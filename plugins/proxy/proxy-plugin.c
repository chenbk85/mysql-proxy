/* Copyright (C) 2007, 2008 MySQL AB */ 

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/** 
 * @page proxy_states The internal states of the Proxy
 *
 * The MySQL Proxy implements the MySQL Protocol in its own way. 
 *
 *   -# connect @msc
 *   client, proxy, backend;
 *   --- [ label = "connect to backend" ];
 *   client->proxy  [ label = "INIT" ];
 *   proxy->backend [ label = "CONNECT_SERVER", URL="\ref proxy_connect_server" ];
 * @endmsc
 *   -# auth @msc
 *   client, proxy, backend;
 *   --- [ label = "authenticate" ];
 *   backend->proxy [ label = "READ_HANDSHAKE", URL="\ref proxy_read_handshake" ];
 *   proxy->client  [ label = "SEND_HANDSHAKE" ];
 *   client->proxy  [ label = "READ_AUTH", URL="\ref proxy_read_auth" ];
 *   proxy->backend [ label = "SEND_AUTH" ];
 *   backend->proxy [ label = "READ_AUTH_RESULT", URL="\ref proxy_read_auth_result" ];
 *   proxy->client  [ label = "SEND_AUTH_RESULT" ];
 * @endmsc
 *   -# query @msc
 *   client, proxy, backend;
 *   --- [ label = "query result phase" ];
 *   client->proxy  [ label = "READ_QUERY", URL="\ref proxy_read_query" ];
 *   proxy->backend [ label = "SEND_QUERY" ];
 *   backend->proxy [ label = "READ_QUERY_RESULT", URL="\ref proxy_read_query_result" ];
 *   proxy->client  [ label = "SEND_QUERY_RESULT", URL="\ref proxy_send_query_result" ];
 * @endmsc
 *
 *   - network_mysqld_proxy_connection_init()
 *     -# registers the callbacks 
 *   - proxy_connect_server() (CON_STATE_CONNECT_SERVER)
 *     -# calls the connect_server() function in the lua script which might decide to
 *       -# send a handshake packet without contacting the backend server (CON_STATE_SEND_HANDSHAKE)
 *       -# closing the connection (CON_STATE_ERROR)
 *       -# picking a active connection from the connection pool
 *       -# pick a backend to authenticate against
 *       -# do nothing 
 *     -# by default, pick a backend from the backend list on the backend with the least active connctions
 *     -# opens the connection to the backend with connect()
 *     -# when done CON_STATE_READ_HANDSHAKE 
 *   - proxy_read_handshake() (CON_STATE_READ_HANDSHAKE)
 *     -# reads the handshake packet from the server 
 *   - proxy_read_auth() (CON_STATE_READ_AUTH)
 *     -# reads the auth packet from the client 
 *   - proxy_read_auth_result() (CON_STATE_READ_AUTH_RESULT)
 *     -# reads the auth-result packet from the server 
 *   - proxy_send_auth_result() (CON_STATE_SEND_AUTH_RESULT)
 *   - proxy_read_query() (CON_STATE_READ_QUERY)
 *     -# reads the query from the client 
 *   - proxy_read_query_result() (CON_STATE_READ_QUERY_RESULT)
 *     -# reads the query-result from the server 
 *   - proxy_send_query_result() (CON_STATE_SEND_QUERY_RESULT)
 *     -# called after the data is written to the client
 *     -# if scripts wants to close connections, goes to CON_STATE_ERROR
 *     -# if queries are in the injection queue, goes to CON_STATE_SEND_QUERY
 *     -# otherwise goes to CON_STATE_READ_QUERY
 *     -# does special handling for COM_BINLOG_DUMP (go to CON_STATE_READ_QUERY_RESULT) 

 */

#ifdef HAVE_SYS_FILIO_H
/**
 * required for FIONREAD on solaris
 */
#include <sys/filio.h>
#endif

#ifndef _WIN32
#include <sys/ioctl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define ioctlsocket ioctl
#endif

#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>

#include <errno.h>

#include <glib.h>

#ifdef HAVE_LUA_H
/**
 * embedded lua support
 */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#endif

/* for solaris 2.5 and NetBSD 1.3.x */
#ifndef HAVE_SOCKLEN_T
typedef int socklen_t;
#endif


#include <mysqld_error.h> /** for ER_UNKNOWN_ERROR */

#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "network-mysqld-packet.h"

#include "network-mysqld-lua.h"

#include "network-conn-pool.h"
#include "network-conn-pool-lua.h"

#include "sys-pedantic.h"
#include "query-handling.h"
#include "backend.h"
#include "glib-ext.h"
#include "lua-env.h"

#include "proxy-plugin.h"

#include "sql-tokenizer.h"
#include "lua-load-factory.h"

#define C(x) x, sizeof(x) - 1
#define S(x) x->str, x->len

#define HASH_INSERT(hash, key, expr) \
		do { \
			GString *hash_value; \
			if ((hash_value = g_hash_table_lookup(hash, key))) { \
				expr; \
			} else { \
				hash_value = g_string_new(NULL); \
				expr; \
				g_hash_table_insert(hash, g_strdup(key), hash_value); \
			} \
		} while(0);

#define CRASHME() do { char *_crashme = NULL; *_crashme = 0; } while(0);

struct chassis_plugin_config {
	gchar *address;                   /**< listening address of the proxy */

	gchar **backend_addresses;        /**< read-write backends */
	gchar **read_only_backend_addresses; /**< read-only  backends */

	gint fix_bug_25371;               /**< suppress the second ERR packet of bug #25371 */

	gint profiling;                   /**< skips the execution of the read_query() function */
	
	gchar *lua_script;                /**< script to load at the start the connection */

	gint pool_change_user;            /**< don't reset the connection, when a connection is taken from the pool
					       - this safes a round-trip, but we also don't cleanup the connection
					       - another name could be "fast-pool-connect", but that's too friendly
					       */

	gint start_proxy;

	network_mysqld_con *listen_con;
};


static network_mysqld_lua_stmt_ret proxy_lua_read_query_result(network_mysqld_con *con) {
	network_socket *send_sock = con->client;
	injection *inj = NULL;
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	network_mysqld_lua_stmt_ret ret = PROXY_NO_DECISION;

	/**
	 * check if we want to forward the statement to the client 
	 *
	 * if not, clean the send-queue 
	 */

	if (0 == st->injected.queries->length) return PROXY_NO_DECISION;

	inj = g_queue_pop_head(st->injected.queries);

#ifdef HAVE_LUA_H
	/* call the lua script to pick a backend
	 * */
	network_mysqld_con_lua_register_callback(con, con->config->lua_script);

	if (st->L) {
		lua_State *L = st->L;

		g_assert(lua_isfunction(L, -1));
		lua_getfenv(L, -1);
		g_assert(lua_istable(L, -1));
		
		lua_getfield_literal(L, -1, C("read_query_result"));
		if (lua_isfunction(L, -1)) {
			injection **inj_p;
			GString *packet;

			inj_p = lua_newuserdata(L, sizeof(inj));
			*inj_p = inj;

			inj->result_queue = con->client->send_queue->chunks;
			inj->qstat = st->injected.qstat;

			proxy_getinjectionmetatable(L);
			lua_setmetatable(L, -2);

			if (lua_pcall(L, 1, 1, 0) != 0) {
				g_critical("(read_query_result) %s", lua_tostring(L, -1));

				lua_pop(L, 1); /* err-msg */

				ret = PROXY_NO_DECISION;
			} else {
				if (lua_isnumber(L, -1)) {
					ret = lua_tonumber(L, -1);
				}
				lua_pop(L, 1);
			}

			switch (ret) {
			case PROXY_SEND_RESULT:
				/**
				 * replace the result-set the server sent us 
				 */
				while ((packet = g_queue_pop_head(send_sock->send_queue->chunks))) g_string_free(packet, TRUE);
				
				/**
				 * we are a response to the client packet, hence one packet id more 
				 */
				send_sock->packet_id++;

				if (network_mysqld_con_lua_handle_proxy_response(con, con->config->lua_script)) {
					/**
					 * handling proxy.response failed
					 *
					 * send a ERR packet in case there was no result-set sent yet
					 */
			
					if (!st->injected.sent_resultset) {
						network_mysqld_con_send_error(con->client, C("(lua) handling proxy.response failed, check error-log"));
					}
				}

				/* fall through */
			case PROXY_NO_DECISION:
				if (!st->injected.sent_resultset) {
					/**
					 * make sure we send only one result-set per client-query
					 */
					st->injected.sent_resultset++;
					break;
				}
				g_warning("%s.%d: got asked to send a resultset, but ignoring it as we already have sent %d resultset(s). injection-id: %d",
						__FILE__, __LINE__,
						st->injected.sent_resultset,
						inj->id);

				st->injected.sent_resultset++;

				/* fall through */
			case PROXY_IGNORE_RESULT:
				/* trash the packets for the injection query */
				while ((packet = g_queue_pop_head(send_sock->send_queue->chunks))) g_string_free(packet, TRUE);

				break;
			default:
				/* invalid return code */
				g_message("%s.%d: return-code for read_query_result() was neither PROXY_SEND_RESULT or PROXY_IGNORE_RESULT, will ignore the result",
						__FILE__, __LINE__);

				while ((packet = g_queue_pop_head(send_sock->send_queue->chunks))) g_string_free(packet, TRUE);

				break;
			}

		} else if (lua_isnil(L, -1)) {
			/* no function defined, let's send the result-set */
			lua_pop(L, 1); /* pop the nil */
		} else {
			g_message("%s.%d: (network_mysqld_con_handle_proxy_resultset) got wrong type: %s", __FILE__, __LINE__, lua_typename(L, lua_type(L, -1)));
			lua_pop(L, 1); /* pop the nil */
		}
		lua_pop(L, 1); /* fenv */

		g_assert(lua_isfunction(L, -1));
	}
#endif

	injection_free(inj);

	return ret;
}

/**
 * call the lua function to intercept the handshake packet
 *
 * @return PROXY_SEND_QUERY  to send the packet from the client
 *         PROXY_NO_DECISION to pass the server packet unmodified
 */
static network_mysqld_lua_stmt_ret proxy_lua_read_handshake(network_mysqld_con *con) {
	network_mysqld_lua_stmt_ret ret = PROXY_NO_DECISION; /* send what the server gave us */
#ifdef HAVE_LUA_H
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	network_socket   *recv_sock = con->server;
	network_socket   *send_sock = con->client;

	lua_State *L;

	/* call the lua script to pick a backend
	 * */
	network_mysqld_con_lua_register_callback(con, con->config->lua_script);

	if (!st->L) return ret;

	L = st->L;

	g_assert(lua_isfunction(L, -1));
	lua_getfenv(L, -1);
	g_assert(lua_istable(L, -1));
	
	lua_getfield_literal(L, -1, C("read_handshake"));
	if (lua_isfunction(L, -1)) {
		/* export
		 *
		 * every thing we know about it
		 *  */

		if (lua_pcall(L, 0, 1, 0) != 0) {
			g_critical("(read_handshake) %s", lua_tostring(L, -1));

			lua_pop(L, 1); /* errmsg */

			/* the script failed, but we have a useful default */
		} else {
			if (lua_isnumber(L, -1)) {
				ret = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
		}
	
		switch (ret) {
		case PROXY_NO_DECISION:
			break;
		case PROXY_SEND_QUERY:
			g_warning("%s.%d: (read_handshake) return proxy.PROXY_SEND_QUERY is deprecated, use PROXY_SEND_RESULT instead",
					__FILE__, __LINE__);

			ret = PROXY_SEND_RESULT;
		case PROXY_SEND_RESULT:
			/**
			 * proxy.response.type = ERR, RAW, ...
			 */

			if (network_mysqld_con_lua_handle_proxy_response(con, con->config->lua_script)) {
				/**
				 * handling proxy.response failed
				 *
				 * send a ERR packet
				 */
		
				network_mysqld_con_send_error(con->client, C("(lua) handling proxy.response failed, check error-log"));
			}

			break;
		default:
			ret = PROXY_NO_DECISION;
			break;
		}
	} else if (lua_isnil(L, -1)) {
		lua_pop(L, 1); /* pop the nil */
	} else {
		g_message("%s.%d: %s", __FILE__, __LINE__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1); /* pop the ... */
	}
	lua_pop(L, 1); /* fenv */

	g_assert(lua_isfunction(L, -1));
#endif
	return ret;
}


/**
 * parse the hand-shake packet from the server
 *
 *
 * @note the SSL and COMPRESS flags are disabled as we can't 
 *       intercept or parse them.
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_handshake) {
	network_packet packet;
	network_socket *recv_sock, *send_sock;
	network_mysqld_auth_challenge *challenge;
	GString *challenge_packet;

	send_sock = con->client;
	recv_sock = con->server;

 	packet.data = g_queue_peek_tail(recv_sock->recv_queue->chunks);
	packet.offset = 0;

	challenge = network_mysqld_auth_challenge_new();
	if (network_mysqld_proto_get_auth_challenge(&packet, challenge)) {
		recv_sock->packet_len = PACKET_LEN_UNSET;
 		g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

		network_mysqld_auth_challenge_free(challenge);

		return NETWORK_SOCKET_ERROR;
	}

 	con->server->challenge = challenge;

	/* we can't sniff compressed packets nor do we support SSL */
	challenge->capabilities &= ~(CLIENT_COMPRESS);
	challenge->capabilities &= ~(CLIENT_SSL);

	switch (proxy_lua_read_handshake(con)) {
	case PROXY_NO_DECISION:
		break;
	case PROXY_SEND_QUERY:
		/* the client overwrote and wants to send its own packet
		 * it is already in the queue */

		recv_sock->packet_len = PACKET_LEN_UNSET;
 		g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

		return NETWORK_SOCKET_ERROR;
	default:
		g_error("%s.%d: ...", __FILE__, __LINE__);
		break;
	} 

	challenge_packet = g_string_new(NULL);
	network_mysqld_proto_append_auth_challenge(challenge_packet, challenge);
	network_mysqld_queue_append(send_sock->send_queue, S(challenge_packet), recv_sock->packet_id);

	g_string_free(challenge_packet, TRUE);

	recv_sock->packet_len = PACKET_LEN_UNSET;
	g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

	/* copy the pack to the client */
	con->state = CON_STATE_SEND_HANDSHAKE;

	return NETWORK_SOCKET_SUCCESS;
}

static network_mysqld_lua_stmt_ret proxy_lua_read_auth(network_mysqld_con *con) {
	network_mysqld_lua_stmt_ret ret = PROXY_NO_DECISION;

#ifdef HAVE_LUA_H
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	lua_State *L;

	/* call the lua script to pick a backend
	 * */
	network_mysqld_con_lua_register_callback(con, con->config->lua_script);

	if (!st->L) return 0;

	L = st->L;

	g_assert(lua_isfunction(L, -1));
	lua_getfenv(L, -1);
	g_assert(lua_istable(L, -1));
	
	lua_getfield_literal(L, -1, C("read_auth"));
	if (lua_isfunction(L, -1)) {

		/* export
		 *
		 * every thing we know about it
		 *  */

		if (lua_pcall(L, 0, 1, 0) != 0) {
			g_critical("(read_auth) %s", lua_tostring(L, -1));

			lua_pop(L, 1); /* errmsg */

			/* the script failed, but we have a useful default */
		} else {
			if (lua_isnumber(L, -1)) {
				ret = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
		}

		switch (ret) {
		case PROXY_NO_DECISION:
			break;
		case PROXY_SEND_RESULT:
			/* answer directly */

			con->client->packet_id++;
			
			if (network_mysqld_con_lua_handle_proxy_response(con, con->config->lua_script)) {
				/**
				 * handling proxy.response failed
				 *
				 * send a ERR packet
				 */
		
				network_mysqld_con_send_error(con->client, C("(lua) handling proxy.response failed, check error-log"));
			}

			break;
		default:
			ret = PROXY_NO_DECISION;
			break;
		}

		/* ret should be a index into */

	} else if (lua_isnil(L, -1)) {
		lua_pop(L, 1); /* pop the nil */
	} else {
		g_message("%s.%d: %s", __FILE__, __LINE__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1); /* pop the ... */
	}
	lua_pop(L, 1); /* fenv */

	g_assert(lua_isfunction(L, -1));
#endif
	return ret;
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_auth) {
	/* read auth from client */
	network_packet packet;
	GList *chunk;
	network_socket *recv_sock, *send_sock;
	chassis_plugin_config *config = con->config;
	network_mysqld_auth_response *auth;

	recv_sock = con->client;
	send_sock = con->server;

	chunk = recv_sock->recv_queue->chunks->tail;

	packet.data = chunk->data;
	packet.offset = 0;

	if (packet.data->len != recv_sock->packet_len + NET_HEADER_SIZE) return NETWORK_SOCKET_SUCCESS; /* we are not finished yet */

	auth = network_mysqld_auth_response_new();

	if (network_mysqld_proto_get_auth_response(&packet, auth)) {
		network_mysqld_auth_response_free(auth);
		return NETWORK_SOCKET_ERROR;
	}

 	con->client->response = auth;

	g_string_assign_len(con->client->default_db, S(auth->database));

	/**
	 * looks like we finished parsing, call the lua function
	 */
	switch (proxy_lua_read_auth(con)) {
	case PROXY_SEND_RESULT:
		con->state = CON_STATE_SEND_AUTH_RESULT;

		g_string_free(packet.data, TRUE);
		chunk->data = NULL;

		break;
	case PROXY_NO_DECISION:
		/* if we don't have a backend (con->server), we just ack the client auth
		 */
		if (!con->server) {
			GString *auth_resp_packet;

			con->state = CON_STATE_SEND_AUTH_RESULT;
		
			chunk->data = NULL;

			auth_resp_packet = g_string_new(NULL);

			network_mysqld_proto_append_ok_packet(auth_resp_packet, 0, 0, 2 /* we should track this flag in the pool */, 0);

			network_mysqld_queue_append(recv_sock->send_queue, 
					S(auth_resp_packet), 2);

			g_string_free(auth_resp_packet, TRUE);

			break;
		}
		/* if the server-side of the connection is already up and authed
		 * we send a COM_CHANGE_USER to reauth the connection and remove
		 * all temp-tables and session-variables
		 *
		 * for performance reasons this extra reauth can be disabled. But
		 * that leaves temp-tables on the connection.
		 */
		if (con->server->is_authed) {
			if (config->pool_change_user) {
				GString *com_change_user = g_string_new(NULL);
				/* copy incl. the nul */
				g_string_append_c(com_change_user, COM_CHANGE_USER);
				g_string_append_len(com_change_user, con->client->response->username->str, con->client->response->username->len + 1); /* nul-term */

				g_assert_cmpint(con->client->response->response->len, <, 250);

				g_string_append_c(com_change_user, (con->client->response->response->len & 0xff));
				g_string_append_len(com_change_user, S(con->client->response->response));

				g_string_append_len(com_change_user, con->client->default_db->str, con->client->default_db->len + 1);
				
				network_mysqld_queue_append(send_sock->send_queue, 
						S(com_change_user), 
						0);

				/**
				 * the server is already authenticated, the client isn't
				 *
				 * transform the auth-packet into a COM_CHANGE_USER
				 */

				g_string_free(com_change_user, TRUE);
			
				con->state = CON_STATE_SEND_AUTH;
			} else {
				GString *auth_resp;
				/* check if the username and client-scramble are the same as in the previous authed
				 * connection */

				auth_resp = g_string_new(NULL);

				con->state = CON_STATE_SEND_AUTH_RESULT;

				if (!g_string_equal(con->client->response->username, con->server->response->username) ||
				    !g_string_equal(con->client->response->response, con->server->response->response)) {
					network_mysqld_proto_append_error_packet(auth_resp, C("(proxy-pool) login failed"), ER_ACCESS_DENIED_ERROR, "28000");
				} else {
					network_mysqld_proto_append_ok_packet(auth_resp, 0, 0, 2 /* we should track this flag in the pool */, 0);
				}

				network_mysqld_queue_append(recv_sock->send_queue, 
						S(auth_resp), 
						2);

				g_string_free(auth_resp, TRUE);
			}

			/* free the packet as we don't forward it */
			g_string_free(packet.data, TRUE);
			chunk->data = NULL;
		} else {
			network_queue_append(send_sock->send_queue, packet.data);
			con->state = CON_STATE_SEND_AUTH;
		}

		break;
	default:
		g_error("%s.%d: ... ", __FILE__, __LINE__);
		break;
	}

	recv_sock->packet_len = PACKET_LEN_UNSET;
	g_queue_delete_link(recv_sock->recv_queue->chunks, chunk);

	return NETWORK_SOCKET_SUCCESS;
}

static network_mysqld_lua_stmt_ret proxy_lua_read_auth_result(network_mysqld_con *con) {
	network_mysqld_lua_stmt_ret ret = PROXY_NO_DECISION;

#ifdef HAVE_LUA_H
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	network_socket *recv_sock = con->server;
	GList *chunk = recv_sock->recv_queue->chunks->tail;
	GString *packet = chunk->data;
	lua_State *L;

	/* call the lua script to pick a backend
	 * */
	network_mysqld_con_lua_register_callback(con, con->config->lua_script);

	if (!st->L) return 0;

	L = st->L;

	g_assert(lua_isfunction(L, -1));
	lua_getfenv(L, -1);
	g_assert(lua_istable(L, -1));
	
	lua_getfield_literal(L, -1, C("read_auth_result"));
	if (lua_isfunction(L, -1)) {

		/* export
		 *
		 * every thing we know about it
		 *  */

		lua_newtable(L);

		lua_pushlstring(L, packet->str + NET_HEADER_SIZE, packet->len - NET_HEADER_SIZE);
		lua_setfield(L, -2, "packet");

		if (lua_pcall(L, 1, 1, 0) != 0) {
			g_critical("(read_auth_result) %s", lua_tostring(L, -1));

			lua_pop(L, 1); /* errmsg */

			/* the script failed, but we have a useful default */
		} else {
			if (lua_isnumber(L, -1)) {
				ret = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
		}

		switch (ret) {
		case PROXY_NO_DECISION:
			break;
		case PROXY_SEND_RESULT:
			/* answer directly */

			if (network_mysqld_con_lua_handle_proxy_response(con, con->config->lua_script)) {
				/**
				 * handling proxy.response failed
				 *
				 * send a ERR packet
				 */
		
				network_mysqld_con_send_error(con->client, C("(lua) handling proxy.response failed, check error-log"));
			}

			break;
		default:
			ret = PROXY_NO_DECISION;
			break;
		}

		/* ret should be a index into */

	} else if (lua_isnil(L, -1)) {
		lua_pop(L, 1); /* pop the nil */
	} else {
		g_message("%s.%d: %s", __FILE__, __LINE__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1); /* pop the ... */
	}
	lua_pop(L, 1); /* fenv */

	g_assert(lua_isfunction(L, -1));
#endif
	return ret;
}


NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_auth_result) {
	GString *packet;
	GList *chunk;
	network_socket *recv_sock, *send_sock;

	recv_sock = con->server;
	send_sock = con->client;

	chunk = recv_sock->recv_queue->chunks->tail;
	packet = chunk->data;

	/* we aren't finished yet */
	if (packet->len != recv_sock->packet_len + NET_HEADER_SIZE) return NETWORK_SOCKET_SUCCESS;

	/* send the auth result to the client */
	if (con->server->is_authed) {
		/**
		 * we injected a COM_CHANGE_USER above and have to correct to 
		 * packet-id now 
		 */
		packet->str[3] = 2;
	}

	/**
	 * copy the 
	 * - default-db, 
	 * - username, 
	 * - scrambed_password
	 *
	 * to the server-side 
	 */
	g_string_assign_len(recv_sock->default_db, S(send_sock->default_db));

	con->server->response = network_mysqld_auth_response_copy(con->client->response);

	/**
	 * recv_sock still points to the old backend that
	 * we received the packet from. 
	 *
	 * backend_ndx = 0 might have reset con->server
	 */

	switch (proxy_lua_read_auth_result(con)) {
	case PROXY_SEND_RESULT:
		/**
		 * we already have content in the send-sock 
		 *
		 * chunk->packet is not forwarded, free it
		 */

		g_string_free(packet, TRUE);
		
		break;
	case PROXY_NO_DECISION:
		network_queue_append(send_sock->send_queue, packet);

		break;
	default:
		g_error("%s.%d: ... ", __FILE__, __LINE__);
		break;
	}

	/**
	 * we handled the packet on the server side, free it
	 */
	recv_sock->packet_len = PACKET_LEN_UNSET;
	g_queue_delete_link(recv_sock->recv_queue->chunks, chunk);
	
	con->state = CON_STATE_SEND_AUTH_RESULT;

	return NETWORK_SOCKET_SUCCESS;
}

static network_mysqld_lua_stmt_ret proxy_lua_read_query(network_mysqld_con *con) {
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	char command = -1;
	injection *inj;
	network_socket *recv_sock = con->client;
	GList   *chunk  = recv_sock->recv_queue->chunks->head;
	GString *packet = chunk->data;
	chassis_plugin_config *config = con->config;

	if (!config->profiling) return PROXY_SEND_QUERY;

	if (packet->len < NET_HEADER_SIZE) return PROXY_SEND_QUERY; /* packet too short */

	command = packet->str[NET_HEADER_SIZE + 0];

	if (COM_QUERY == command) {
		/* we need some more data after the COM_QUERY */
		if (packet->len < NET_HEADER_SIZE + 2) return PROXY_SEND_QUERY;

		/* LOAD DATA INFILE is nasty */
		if (packet->len - NET_HEADER_SIZE - 1 >= sizeof("LOAD ") - 1 &&
		    0 == g_ascii_strncasecmp(packet->str + NET_HEADER_SIZE + 1, C("LOAD "))) return PROXY_SEND_QUERY;

		/* don't cover them with injected queries as it trashes the result */
		if (packet->len - NET_HEADER_SIZE - 1 >= sizeof("SHOW ERRORS") - 1 &&
		    0 == g_ascii_strncasecmp(packet->str + NET_HEADER_SIZE + 1, C("SHOW ERRORS"))) return PROXY_SEND_QUERY;
		if (packet->len - NET_HEADER_SIZE - 1 >= sizeof("select @@error_count") - 1 &&
		    0 == g_ascii_strncasecmp(packet->str + NET_HEADER_SIZE + 1, C("select @@error_count"))) return PROXY_SEND_QUERY;
	
	}

	/* reset the query status */
	memset(&(st->injected.qstat), 0, sizeof(st->injected.qstat));
	
	while ((inj = g_queue_pop_head(st->injected.queries))) injection_free(inj);

	/* ok, here we go */

#ifdef HAVE_LUA_H
	network_mysqld_con_lua_register_callback(con, con->config->lua_script);

	if (st->L) {
		lua_State *L = st->L;
		network_mysqld_lua_stmt_ret ret = PROXY_NO_DECISION;

		g_assert(lua_isfunction(L, -1));
		lua_getfenv(L, -1);
		g_assert(lua_istable(L, -1));

		/**
		 * reset proxy.response to a empty table 
		 */
		lua_getfield(L, -1, "proxy");
		g_assert(lua_istable(L, -1));

		lua_newtable(L);
		lua_setfield(L, -2, "response");

		lua_pop(L, 1);
		
		/**
		 * get the call back
		 */
		lua_getfield_literal(L, -1, C("read_query"));
		if (lua_isfunction(L, -1)) {

			/* pass the packet as parameter */
			lua_pushlstring(L, packet->str + NET_HEADER_SIZE, packet->len - NET_HEADER_SIZE);

			if (lua_pcall(L, 1, 1, 0) != 0) {
				/* hmm, the query failed */
				g_critical("(read_query) %s", lua_tostring(L, -1));

				lua_pop(L, 2); /* fenv + errmsg */

				/* perhaps we should clean up ?*/

				return PROXY_SEND_QUERY;
			} else {
				if (lua_isnumber(L, -1)) {
					ret = lua_tonumber(L, -1);
				}
				lua_pop(L, 1);
			}

			switch (ret) {
			case PROXY_SEND_RESULT:
				/* check the proxy.response table for content,
				 *
				 */
	
				con->client->packet_id++;

				if (network_mysqld_con_lua_handle_proxy_response(con, con->config->lua_script)) {
					/**
					 * handling proxy.response failed
					 *
					 * send a ERR packet
					 */
			
					network_mysqld_con_send_error(con->client, C("(lua) handling proxy.response failed, check error-log"));
				}
	
				break;
			case PROXY_NO_DECISION:
				/**
				 * PROXY_NO_DECISION and PROXY_SEND_QUERY may pick another backend
				 */
				break;
			case PROXY_SEND_QUERY:
				/* send the injected queries
				 *
				 * injection_init(..., query);
				 * 
				 *  */

				if (st->injected.queries->length) {
					ret = PROXY_SEND_INJECTION;
				}
	
				break;
			default:
				break;
			}
			lua_pop(L, 1); /* fenv */
		} else {
			lua_pop(L, 2); /* fenv + nil */
		}

		g_assert(lua_isfunction(L, -1));

		if (ret != PROXY_NO_DECISION) {
			return ret;
		}
	}
#endif
	return PROXY_NO_DECISION;
}

/**
 * gets called after a query has been read
 *
 * - calls the lua script via network_mysqld_con_handle_proxy_stmt()
 *
 * @see network_mysqld_con_handle_proxy_stmt
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_query) {
	GString *packet;
	GList *chunk;
	network_socket *recv_sock, *send_sock;
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	int proxy_query = 1;
	network_mysqld_lua_stmt_ret ret;

	send_sock = NULL;
	recv_sock = con->client;
	st->injected.sent_resultset = 0;

	chunk = recv_sock->recv_queue->chunks->head;

	if (recv_sock->recv_queue->chunks->length != 1) {
		g_message("%s.%d: client-recv-queue-len = %d", __FILE__, __LINE__, recv_sock->recv_queue->chunks->length);
	}
	
	packet = chunk->data;

	if (packet->len != recv_sock->packet_len + NET_HEADER_SIZE) return NETWORK_SOCKET_SUCCESS;

	con->parse.len = recv_sock->packet_len;

	ret = proxy_lua_read_query(con);

	/**
	 * if we disconnected in read_query_result() we have no connection open
	 * when we try to execute the next query 
	 *
	 * for PROXY_SEND_RESULT we don't need a server
	 */
	if (ret != PROXY_SEND_RESULT &&
	    con->server == NULL) {
		g_critical("%s.%d: I have no server backend, closing connection", __FILE__, __LINE__);
		return NETWORK_SOCKET_ERROR;
	}
	
	send_sock = con->server;

	switch (ret) {
	case PROXY_NO_DECISION:
	case PROXY_SEND_QUERY:
		/* no injection, pass on the chunk as is */
		send_sock->packet_id = recv_sock->packet_id;

		network_queue_append(send_sock->send_queue, packet);

		break;
	case PROXY_SEND_RESULT: 
		proxy_query = 0;
		
		g_string_free(chunk->data, TRUE);

		break; 
	case PROXY_SEND_INJECTION: {
		injection *inj;

		inj = g_queue_peek_head(st->injected.queries);

		/* there might be no query, if it was banned */
		network_mysqld_queue_append(send_sock->send_queue, inj->query->str, inj->query->len, 0);

		g_string_free(chunk->data, TRUE);

		break; }
	default:
		g_error("%s.%d: ", __FILE__, __LINE__);
	}

	recv_sock->packet_len = PACKET_LEN_UNSET;
	g_queue_delete_link(recv_sock->recv_queue->chunks, chunk);

	if (proxy_query) {
		con->state = CON_STATE_SEND_QUERY;
	} else {
		con->state = CON_STATE_SEND_QUERY_RESULT;
	}

	return NETWORK_SOCKET_SUCCESS;
}

/**
 * decide about the next state after the result-set has been written 
 * to the client
 * 
 * if we still have data in the queue, back to proxy_send_query()
 * otherwise back to proxy_read_query() to pick up a new client query
 *
 * @note we should only send one result back to the client
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_send_query_result) {
	network_socket *recv_sock, *send_sock;
	injection *inj;
	network_mysqld_con_lua_t *st = con->plugin_con_state;

	send_sock = con->server;
	recv_sock = con->client;

	if (st->connection_close) {
		con->state = CON_STATE_ERROR;

		return NETWORK_SOCKET_SUCCESS;
	}

	if (con->parse.command == COM_BINLOG_DUMP) {
		/**
		 * the binlog dump is different as it doesn't have END packet
		 *
		 * @todo in 5.0.x a NON_BLOCKING option as added which sends a EOF
		 */
		con->state = CON_STATE_READ_QUERY_RESULT;

		return NETWORK_SOCKET_SUCCESS;
	}

	/* if we don't have a backend, don't try to forward queries
	 */
	if (!send_sock) {
		while ((inj = g_queue_pop_head(st->injected.queries))) {
			g_debug_hexdump(G_STRLOC " proxy.queries:append() without a server-backend", S(inj->query));

			injection_free(inj);
		}
	}

	if (st->injected.queries->length == 0) {
		con->state = CON_STATE_READ_QUERY;

		return NETWORK_SOCKET_SUCCESS;
	}

	con->parse.len = recv_sock->packet_len;

	/* looks like we still have queries in the queue, 
	 * push the next one 
	 */
	inj = g_queue_peek_head(st->injected.queries);

	g_assert(inj);
	g_assert(send_sock);

	network_mysqld_queue_append(send_sock->send_queue, S(inj->query), 0);

	network_mysqld_con_reset_command_response_state(con);

	con->state = CON_STATE_SEND_QUERY;

	return NETWORK_SOCKET_SUCCESS;
}

/**
 * handle the query-result we received from the server
 *
 * - decode the result-set to track if we are finished already
 * - handles BUG#25371 if requested
 * - if the packet is finished, calls the network_mysqld_con_handle_proxy_resultset
 *   to handle the resultset in the lua-scripts
 *
 * @see network_mysqld_con_handle_proxy_resultset
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_query_result) {
	int is_finished = 0;
	network_packet packet;
	GList *chunk;
	network_socket *recv_sock, *send_sock;
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	injection *inj = NULL;
	chassis_plugin_config *config = con->config;
	gint8 status;

	recv_sock = con->server;
	send_sock = con->client;

	chunk = recv_sock->recv_queue->chunks->tail;
	packet.data = chunk->data;
	packet.offset = 0;

	/**
	 * check if we want to forward the statement to the client 
	 *
	 * if not, clean the send-queue 
	 */

	if (0 != st->injected.queries->length) {
		inj = g_queue_peek_head(st->injected.queries);
	}

	if (inj && inj->ts_read_query_result_first.tv_sec == 0) {
		/**
		 * log the time of the first received packet
		 */
		g_get_current_time(&(inj->ts_read_query_result_first));
	}

	if (packet.data->len != recv_sock->packet_len + NET_HEADER_SIZE) return NETWORK_SOCKET_SUCCESS;

	is_finished = network_mysqld_proto_get_query_result(&packet, con);

	network_queue_append(send_sock->send_queue, packet.data);

	g_queue_delete_link(recv_sock->recv_queue->chunks, chunk);
	recv_sock->packet_len = PACKET_LEN_UNSET;

	if (is_finished) {
		/**
		 * the resultset handler might decide to trash the send-queue
		 * 
		 * */

		if (inj) {
			if (con->parse.command == COM_QUERY) {
				network_mysqld_com_query_result_t *com_query = con->parse.data;

				inj->bytes = com_query->bytes;
				inj->rows  = com_query->rows;
			}
			g_get_current_time(&(inj->ts_read_query_result_last));
		}

		proxy_lua_read_query_result(con);

		/** recv_sock might be != con->server now */

		/**
		 * if the send-queue is empty, we have nothing to send
		 * and can read the next query */	
		if (send_sock->send_queue->chunks) {
			con->state = CON_STATE_SEND_QUERY_RESULT;
		} else {
			con->state = CON_STATE_READ_QUERY;
		}
	}
	
	return NETWORK_SOCKET_SUCCESS;
}

static network_mysqld_lua_stmt_ret proxy_lua_connect_server(network_mysqld_con *con) {
	network_mysqld_lua_stmt_ret ret = PROXY_NO_DECISION;

#ifdef HAVE_LUA_H
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	lua_State *L;

	/* call the lua script to pick a backend
	 * */
	network_mysqld_con_lua_register_callback(con, con->config->lua_script);

	if (!st->L) return 0;

	L = st->L;

	g_assert(lua_isfunction(L, -1));
	lua_getfenv(L, -1);
	g_assert(lua_istable(L, -1));
	
	lua_getfield_literal(L, -1, C("connect_server"));
	if (lua_isfunction(L, -1)) {
		if (lua_pcall(L, 0, 1, 0) != 0) {
			g_critical("%s: (connect_server) %s", 
					G_STRLOC,
					lua_tostring(L, -1));

			lua_pop(L, 1); /* errmsg */

			/* the script failed, but we have a useful default */
		} else {
			if (lua_isnumber(L, -1)) {
				ret = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
		}

		switch (ret) {
		case PROXY_NO_DECISION:
		case PROXY_IGNORE_RESULT:
			break;
		case PROXY_SEND_RESULT:
			/* answer directly */

			if (network_mysqld_con_lua_handle_proxy_response(con, con->config->lua_script)) {
				/**
				 * handling proxy.response failed
				 *
				 * send a ERR packet
				 */
		
				network_mysqld_con_send_error(con->client, C("(lua) handling proxy.response failed, check error-log"));
			}

			break;
		default:
			ret = PROXY_NO_DECISION;
			break;
		}

		/* ret should be a index into */

	} else if (lua_isnil(L, -1)) {
		lua_pop(L, 1); /* pop the nil */
	} else {
		g_message("%s.%d: %s", __FILE__, __LINE__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1); /* pop the ... */
	}
	lua_pop(L, 1); /* fenv */

	g_assert(lua_isfunction(L, -1));
#endif
	return ret;
}



/**
 * connect to a backend
 *
 * @return
 *   NETWORK_SOCKET_SUCCESS        - connected successfully
 *   NETWORK_SOCKET_ERROR_RETRY    - connecting backend failed, call again to connect to another backend
 *   NETWORK_SOCKET_ERROR          - no backends available, adds a ERR packet to the client queue
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_connect_server) {
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	chassis_private *g = con->srv->priv;
	guint min_connected_clients = G_MAXUINT;
	guint i;
	gboolean use_pooled_connection = FALSE;
	backend_t *cur;

	if (con->server) {
		int so_error = 0;
		socklen_t so_error_len = sizeof(so_error);

		/**
		 * we might get called a 2nd time after a connect() == EINPROGRESS
		 */
		if (getsockopt(con->server->fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len)) {
			/* getsockopt failed */
			g_critical("%s.%d: getsockopt(%s) failed: %s", 
					__FILE__, __LINE__,
					con->server->addr.str, strerror(errno));
			return NETWORK_SOCKET_ERROR;
		}

		switch (so_error) {
		case 0:
			break;
		default:
			g_message("%s.%d: connect(%s) failed: %s. Retrying with different backend.", 
					__FILE__, __LINE__,
					con->server->addr.str, strerror(so_error));

			/* mark the backend as being DOWN and retry with a different one */
			st->backend->state = BACKEND_STATE_DOWN;
			network_socket_free(con->server);
			con->server = NULL;	
			return NETWORK_SOCKET_ERROR_RETRY;
		}

		if (st->backend->state != BACKEND_STATE_UP) {
			st->backend->state = BACKEND_STATE_UP;
			g_get_current_time(&(st->backend->state_since));
		}

		con->state = CON_STATE_READ_HANDSHAKE;

		return NETWORK_SOCKET_SUCCESS;
	}

	st->backend = NULL;
	st->backend_ndx = -1;

	network_backends_check(g->backends);

	switch (proxy_lua_connect_server(con)) {
	case PROXY_SEND_RESULT:
		/* we answered directly ... like denial ...
		 *
		 * for sure we have something in the send-queue 
		 *
		 */
		
		return NETWORK_SOCKET_SUCCESS;
	case PROXY_NO_DECISION:
		/* just go on */

		break;
	case PROXY_IGNORE_RESULT:
		use_pooled_connection = TRUE;

		break;
	default:
		g_error("%s.%d: ... ", __FILE__, __LINE__);
		break;
	}

	/* protect the typecast below */
	g_assert_cmpint(g->backends->backends->len, <, G_MAXINT);

	/**
	 * if the current backend is down, ignore it 
	 */
	cur = network_backends_get(g->backends, st->backend_ndx);

	if (cur) {
		if (cur->state == BACKEND_STATE_DOWN) {
			st->backend_ndx = -1;
		}
	}

	if (con->server && !use_pooled_connection) {
		gint bndx = st->backend_ndx;
		/* we already have a connection assigned, 
		 * but the script said we don't want to use it
		 */

		network_connection_pool_lua_add_connection(con);

		st->backend_ndx = bndx;
	}

	if (st->backend_ndx < 0) {
		/**
		 * we can choose between different back addresses 
		 *
		 * prefer SQF (shorted queue first) to load all backends equally
		 */ 

		for (i = 0; i < network_backends_count(g->backends); i++) {
			cur = network_backends_get(g->backends, i);
	
			/**
			 * skip backends which are down or not writable
			 */	
			if (cur->state == BACKEND_STATE_DOWN ||
			    cur->type != BACKEND_TYPE_RW) continue;
	
			if (cur->connected_clients < min_connected_clients) {
				st->backend_ndx = i;
				min_connected_clients = cur->connected_clients;
			}
		}

		if ((cur = network_backends_get(g->backends, st->backend_ndx))) {
			st->backend = cur;
		}
	} else if (NULL == st->backend) {
		if ((cur = network_backends_get(g->backends, st->backend_ndx))) {
			st->backend = cur;
		}
	}

	if (NULL == st->backend) {
		network_mysqld_con_send_error(con->client, C("(proxy) all backends are down"));
		g_critical("%s.%d: Cannot connect, all backends are down.", __FILE__, __LINE__);
		return NETWORK_SOCKET_ERROR;
	}

	/**
	 * check if we have a connection in the pool for this backend
	 */
	if (NULL == con->server) {
		con->server = network_socket_init();
		con->server->addr = st->backend->addr;
		con->server->addr.str = g_strdup(st->backend->addr.str);
	
		st->backend->connected_clients++;

		switch(network_socket_connect(con->server)) {
		case -2:
			/* the socket is non-blocking already, 
			 * call getsockopt() to see if we are done */
			return NETWORK_SOCKET_ERROR_RETRY;
		case 0:
			break;
		default:
			g_message("%s.%d: connecting to backend (%s) failed, marking it as down for ...", 
					__FILE__, __LINE__, con->server->addr.str);

			st->backend->state = BACKEND_STATE_DOWN;
			g_get_current_time(&(st->backend->state_since));

			network_socket_free(con->server);
			con->server = NULL;

			return NETWORK_SOCKET_ERROR_RETRY;
		}

		if (st->backend->state != BACKEND_STATE_UP) {
			st->backend->state = BACKEND_STATE_UP;
			g_get_current_time(&(st->backend->state_since));
		}

		con->state = CON_STATE_READ_HANDSHAKE;
	} else {
		GString *auth_packet;
		/**
		 * send the old hand-shake packet
		 */

		auth_packet = g_string_new(NULL);
		network_mysqld_proto_append_auth_challenge(auth_packet, con->server->challenge);

		/* remove the idle-handler from the socket */
		network_mysqld_queue_append(con->client->send_queue, 
				S(auth_packet),
			       	0); /* packet-id */

		g_string_free(auth_packet, TRUE);
		
		con->state = CON_STATE_SEND_HANDSHAKE;

		/**
		 * connect_clients is already incremented 
		 */
	}

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_init) {
	network_mysqld_con_lua_t *st = con->plugin_con_state;

	g_assert(con->plugin_con_state == NULL);

	st = network_mysqld_con_lua_new();

	con->plugin_con_state = st;
	
	con->state = CON_STATE_CONNECT_SERVER;

	return NETWORK_SOCKET_SUCCESS;
}

static network_mysqld_lua_stmt_ret proxy_lua_disconnect_client(network_mysqld_con *con) {
	network_mysqld_lua_stmt_ret ret = PROXY_NO_DECISION;

#ifdef HAVE_LUA_H
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	lua_State *L;

	/* call the lua script to pick a backend
	 * */
	network_mysqld_con_lua_register_callback(con, con->config->lua_script);

	if (!st->L) return 0;

	L = st->L;

	g_assert(lua_isfunction(L, -1));
	lua_getfenv(L, -1);
	g_assert(lua_istable(L, -1));
	
	lua_getfield_literal(L, -1, C("disconnect_client"));
	if (lua_isfunction(L, -1)) {
		if (lua_pcall(L, 0, 1, 0) != 0) {
			g_critical("%s.%d: (disconnect_client) %s", 
					__FILE__, __LINE__,
					lua_tostring(L, -1));

			lua_pop(L, 1); /* errmsg */

			/* the script failed, but we have a useful default */
		} else {
			if (lua_isnumber(L, -1)) {
				ret = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
		}

		switch (ret) {
		case PROXY_NO_DECISION:
		case PROXY_IGNORE_RESULT:
			break;
		default:
			ret = PROXY_NO_DECISION;
			break;
		}

		/* ret should be a index into */

	} else if (lua_isnil(L, -1)) {
		lua_pop(L, 1); /* pop the nil */
	} else {
		g_message("%s.%d: %s", __FILE__, __LINE__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1); /* pop the ... */
	}
	lua_pop(L, 1); /* fenv */

	g_assert(lua_isfunction(L, -1));
#endif
	return ret;
}


/**
 * cleanup the proxy specific data on the current connection 
 *
 * move the server connection into the connection pool in case it is a 
 * good client-side close
 *
 * @return NETWORK_SOCKET_SUCCESS
 * @see plugin_call_cleanup
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_disconnect_client) {
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	lua_scope  *sc = con->srv->priv->sc;
	gboolean use_pooled_connection = FALSE;

	if (st == NULL) return NETWORK_SOCKET_SUCCESS;
	
	/**
	 * let the lua-level decide if we want to keep the connection in the pool
	 */

	switch (proxy_lua_disconnect_client(con)) {
	case PROXY_NO_DECISION:
		/* just go on */

		break;
	case PROXY_IGNORE_RESULT:
		break;
	default:
		g_error("%s.%d: ... ", __FILE__, __LINE__);
		break;
	}

	/**
	 * check if one of the backends has to many open connections
	 */

	if (use_pooled_connection &&
	    con->state == CON_STATE_CLOSE_CLIENT) {
		/* move the connection to the connection pool
		 *
		 * this disconnects con->server and safes it from getting free()ed later
		 */

		network_connection_pool_lua_add_connection(con);
	} else if (st->backend) {
		/* we have backend assigned and want to close the connection to it */
		st->backend->connected_clients--;
	}

#ifdef HAVE_LUA_H
	/* remove this cached script from registry */
	if (st->L_ref > 0) {
		luaL_unref(sc->L, LUA_REGISTRYINDEX, st->L_ref);
	}
#endif

	network_mysqld_con_lua_free(st);

	con->plugin_con_state = NULL;

	/**
	 * walk all pools and clean them up
	 */

	return NETWORK_SOCKET_SUCCESS;
}

int network_mysqld_proxy_connection_init(network_mysqld_con *con) {
	con->plugins.con_init                      = proxy_init;
	con->plugins.con_connect_server            = proxy_connect_server;
	con->plugins.con_read_handshake            = proxy_read_handshake;
	con->plugins.con_read_auth                 = proxy_read_auth;
	con->plugins.con_read_auth_result          = proxy_read_auth_result;
	con->plugins.con_read_query                = proxy_read_query;
	con->plugins.con_read_query_result         = proxy_read_query_result;
	con->plugins.con_send_query_result         = proxy_send_query_result;
	con->plugins.con_cleanup                   = proxy_disconnect_client;

	return 0;
}

/**
 * free the global scope which is shared between all connections
 *
 * make sure that is called after all connections are closed
 */
void network_mysqld_proxy_free(network_mysqld_con G_GNUC_UNUSED *con) {
}

chassis_plugin_config * network_mysqld_proxy_plugin_init(void) {
	chassis_plugin_config *config;

	config = g_new0(chassis_plugin_config, 1);
	config->fix_bug_25371   = 0; /** double ERR packet on AUTH failures */
	config->profiling       = 1;
	config->start_proxy     = 1;
	config->pool_change_user = 1; /* issue a COM_CHANGE_USER to cleanup the connection 
					 when we get back the connection from the pool */

	return config;
}

void network_mysqld_proxy_plugin_free(chassis_plugin_config *config) {
	gsize i;

	if (config->listen_con) {
		/**
		 * the connection will be free()ed by the network_mysqld_free()
		 */
#if 0
		event_del(&(config->listen_con->server->event));
		network_mysqld_con_free(config->listen_con);
#endif
	}

	if (config->backend_addresses) {
		for (i = 0; config->backend_addresses[i]; i++) {
			g_free(config->backend_addresses[i]);
		}
		g_free(config->backend_addresses);
	}

	if (config->address) {
		/* free the global scope */
		network_mysqld_proxy_free(NULL);

		g_free(config->address);
	}

	if (config->lua_script) g_free(config->lua_script);

	g_free(config);
}

/**
 * plugin options 
 */
static GOptionEntry * network_mysqld_proxy_plugin_get_options(chassis_plugin_config *config) {
	guint i;

	/* make sure it isn't collected */
	static GOptionEntry config_entries[] = 
	{
		{ "proxy-address",            0, 0, G_OPTION_ARG_STRING, NULL, "listening address:port of the proxy-server (default: :4040)", "<host:port>" },
		{ "proxy-read-only-backend-addresses", 
					      0, 0, G_OPTION_ARG_STRING_ARRAY, NULL, "address:port of the remote slave-server (default: not set)", "<host:port>" },
		{ "proxy-backend-addresses",  0, 0, G_OPTION_ARG_STRING_ARRAY, NULL, "address:port of the remote backend-servers (default: 127.0.0.1:3306)", "<host:port>" },
		
		{ "proxy-skip-profiling",     0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, NULL, "disables profiling of queries (default: enabled)", NULL },

		{ "proxy-fix-bug-25371",      0, 0, G_OPTION_ARG_NONE, NULL, "fix bug #25371 (mysqld > 5.1.12) for older libmysql versions", NULL },
		{ "proxy-lua-script",         0, 0, G_OPTION_ARG_STRING, NULL, "filename of the lua script (default: not set)", "<file>" },
		
		{ "no-proxy",                 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, NULL, "don't start the proxy-module (default: enabled)", NULL },
		
		{ "proxy-pool-no-change-user", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, NULL, "don't use CHANGE_USER to reset the connection coming from the pool (default: enabled)", NULL },
		
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

	i = 0;
	config_entries[i++].arg_data = &(config->address);
	config_entries[i++].arg_data = &(config->read_only_backend_addresses);
	config_entries[i++].arg_data = &(config->backend_addresses);

	config_entries[i++].arg_data = &(config->profiling);

	config_entries[i++].arg_data = &(config->fix_bug_25371);
	config_entries[i++].arg_data = &(config->lua_script);
	config_entries[i++].arg_data = &(config->start_proxy);
	config_entries[i++].arg_data = &(config->pool_change_user);

	return config_entries;
}

/**
 * init the plugin with the parsed config
 */
int network_mysqld_proxy_plugin_apply_config(chassis *chas, chassis_plugin_config *config) {
	network_mysqld_con *con;
	network_socket *listen_sock;
	chassis_private *g = chas->priv;
	guint i;

	if (!config->start_proxy) {
		return 0;
	}

	if (!config->address) config->address = g_strdup(":4040");
	if (!config->backend_addresses) {
		config->backend_addresses = g_new0(char *, 2);
		config->backend_addresses[0] = g_strdup("127.0.0.1:3306");
	}

	/** 
	 * create a connection handle for the listen socket 
	 */
	con = network_mysqld_con_init();
	network_mysqld_add_connection(chas, con);
	con->config = config;

	config->listen_con = con;
	
	listen_sock = network_socket_init();
	con->server = listen_sock;

	/* set the plugin hooks as we want to apply them to the new connections too later */
	network_mysqld_proxy_connection_init(con);

	/* FIXME: network_socket_set_address() */
	if (0 != network_address_set_address(&listen_sock->addr, config->address)) {
		return -1;
	}

	/* FIXME: network_socket_bind() */
	if (0 != network_socket_bind(listen_sock)) {
		return -1;
	}

	for (i = 0; config->backend_addresses && config->backend_addresses[i]; i++) {
		network_backends_add(g->backends, config->backend_addresses[i], BACKEND_TYPE_RW);
	}
	
	for (i = 0; config->read_only_backend_addresses && config->read_only_backend_addresses[i]; i++) {
		network_backends_add(g->backends, config->read_only_backend_addresses[i], BACKEND_TYPE_RO);
	}

	/* load the script and setup the global tables */
	network_mysqld_lua_setup_global(chas->priv->sc->L, g);

	/**
	 * call network_mysqld_con_accept() with this connection when we are done
	 */

	event_set(&(listen_sock->event), listen_sock->fd, EV_READ|EV_PERSIST, network_mysqld_con_accept, con);
	event_base_set(chas->event_base, &(listen_sock->event));
	event_add(&(listen_sock->event), NULL);

	return 0;
}

G_MODULE_EXPORT int plugin_init(chassis_plugin *p) {
	p->magic        = CHASSIS_PLUGIN_MAGIC;
	p->name         = g_strdup("proxy");

	p->init         = network_mysqld_proxy_plugin_init;
	p->get_options  = network_mysqld_proxy_plugin_get_options;
	p->apply_config = network_mysqld_proxy_plugin_apply_config;
	p->destroy      = network_mysqld_proxy_plugin_free;

	return 0;
}
