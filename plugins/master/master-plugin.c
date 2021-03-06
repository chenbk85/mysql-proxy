/* $%BEGINLICENSE%$
 Copyright (C) 2007-2008 MySQL AB, 2008 Sun Microsystems, Inc

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 $%ENDLICENSE%$ */
 

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

#include <errno.h>

#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "network-mysqld-packet.h"
#include "network-mysqld-lua.h"

#include "sys-pedantic.h"
#include "glib-ext.h"
#include "lua-env.h"

#include <gmodule.h>

/**
 * do the auth-dance and handle COM_BINLOG_DUMP and the lua code sends binlog-events
 *
 * doing it in lua allows us to read it from any input:
 * - binlogfiles
 * - csv-files 
 * - ...
 */

#define C(x) x, sizeof(x) -1
#define S(x) x->str, x->len

struct chassis_plugin_config {
	gchar *address;                   /**< listening address of the master interface */

	gchar *lua_script;                /**< script to load at the start the connection */

	gchar *master_username;            /**< login username */
	gchar *master_password;            /**< login password */

	network_mysqld_con *listen_con;
};

static int network_mysqld_con_handle_stmt(chassis G_GNUC_UNUSED *chas, network_mysqld_con *con, GString *s) {
	gsize i, j;
	GPtrArray *fields;
	GPtrArray *rows;
	GPtrArray *row;

	
	switch(s->str[NET_HEADER_SIZE]) {
	case COM_QUERY:
		fields = NULL;
		rows = NULL;
		row = NULL;

		if (0 == g_ascii_strncasecmp(s->str + NET_HEADER_SIZE + 1, C("select @@version_comment limit 1"))) {
			MYSQL_FIELD *field;

			fields = network_mysqld_proto_fielddefs_new();

			field = network_mysqld_proto_fielddef_new();
			field->name = g_strdup("@@version_comment");
			field->type = FIELD_TYPE_VAR_STRING;
			g_ptr_array_add(fields, field);

			rows = g_ptr_array_new();
			row = g_ptr_array_new();
			g_ptr_array_add(row, g_strdup("MySQL Enterprise Agent"));
			g_ptr_array_add(rows, row);

			network_mysqld_con_send_resultset(con->client, fields, rows);
			
		} else if (0 == g_ascii_strncasecmp(s->str + NET_HEADER_SIZE + 1, C("select USER()"))) {
			MYSQL_FIELD *field;

			fields = network_mysqld_proto_fielddefs_new();
			field = network_mysqld_proto_fielddef_new();
			field->name = g_strdup("USER()");
			field->type = FIELD_TYPE_VAR_STRING;
			g_ptr_array_add(fields, field);

			rows = g_ptr_array_new();
			row = g_ptr_array_new();
			g_ptr_array_add(row, g_strdup("root"));
			g_ptr_array_add(rows, row);

			network_mysqld_con_send_resultset(con->client, fields, rows);
		} else {
			network_mysqld_con_send_error(con->client, C("(master-server) query not known"));
		}

		/* clean up */
		if (fields) {
			network_mysqld_proto_fielddefs_free(fields);
			fields = NULL;
		}

		if (rows) {
			for (i = 0; i < rows->len; i++) {
				row = rows->pdata[i];

				for (j = 0; j < row->len; j++) {
					g_free(row->pdata[j]);
				}

				g_ptr_array_free(row, TRUE);
			}
			g_ptr_array_free(rows, TRUE);
			rows = NULL;
		}

		break;
	case COM_QUIT:
		break;
	case COM_INIT_DB:
		network_mysqld_con_send_ok(con->client);
		break;
	default:
		network_mysqld_con_send_error(con->client, C("unknown COM_*"));
		break;
	}

	return 0;
}

NETWORK_MYSQLD_PLUGIN_PROTO(server_con_init) {
	network_mysqld_auth_challenge *challenge;
	GString *packet;

	challenge = network_mysqld_auth_challenge_new();
	challenge->server_version_str = g_strdup("5.0.99-master");
	challenge->server_version     = 50099;
	challenge->charset            = 0x08; /* latin1 */
	challenge->capabilities       = CLIENT_PROTOCOL_41 | CLIENT_SECURE_CONNECTION | CLIENT_LONG_PASSWORD;
	challenge->server_status      = SERVER_STATUS_AUTOCOMMIT;
	challenge->thread_id          = 1;

	network_mysqld_auth_challenge_set_challenge(challenge); /* generate a random challenge */

	packet = g_string_new(NULL);
	network_mysqld_proto_append_auth_challenge(packet, challenge);
	con->client->challenge = challenge;

	network_mysqld_queue_append(con->client, con->client->send_queue, S(packet));

	g_string_free(packet, TRUE);
	
	con->state = CON_STATE_SEND_HANDSHAKE;

	g_assert(con->plugin_con_state == NULL);

	con->plugin_con_state = network_mysqld_con_lua_new();

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(server_read_auth) {
	network_packet packet;
	network_socket *recv_sock, *send_sock;
	network_mysqld_auth_response *auth;
	GString *excepted_response;
	
	recv_sock = con->client;
	send_sock = con->client;

	packet.data = g_queue_peek_tail(recv_sock->recv_queue->chunks);
	packet.offset = 0;

	/* decode the packet */
	network_mysqld_proto_skip_network_header(&packet);

	auth = network_mysqld_auth_response_new();
	if (network_mysqld_proto_get_auth_response(&packet, auth)) {
		g_assert_not_reached();
	}
	
	con->client->response = auth;
	
	/* check if the password matches */
	excepted_response = g_string_new(NULL);

	if (!strleq(S(con->client->response->username), con->config->master_username, strlen(con->config->master_username))) {
		network_mysqld_con_send_error_full(send_sock, C("unknown user"), 1045, "28000");
		
		con->state = CON_STATE_SEND_ERROR; /* close the connection after we have sent this packet */
	} else if (network_mysqld_proto_password_scramble(excepted_response,
				S(recv_sock->challenge->challenge),
				con->config->master_password, strlen(con->config->master_password))) {
		network_mysqld_con_send_error_full(send_sock, C("scrambling failed"), 1045, "28000");
		
		con->state = CON_STATE_SEND_ERROR; /* close the connection after we have sent this packet */
	} else if (!g_string_equal(excepted_response, auth->response)) {
		network_mysqld_con_send_error_full(send_sock, C("password doesn't match"), 1045, "28000");
		
		con->state = CON_STATE_SEND_ERROR; /* close the connection after we have sent this packet */
	} else {
		network_mysqld_con_send_ok(send_sock);
	
		con->state = CON_STATE_SEND_AUTH_RESULT;
	}
	
	g_string_free(excepted_response, TRUE);

	g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);
	
	return NETWORK_SOCKET_SUCCESS;
}

static network_mysqld_lua_stmt_ret master_lua_read_query(network_mysqld_con *con) {
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	char command = -1;
	network_socket *recv_sock = con->client;
	GList   *chunk  = recv_sock->recv_queue->chunks->head;
	GString *packet = chunk->data;

	if (packet->len < NET_HEADER_SIZE) return PROXY_SEND_QUERY; /* packet too short */

	command = packet->str[NET_HEADER_SIZE + 0];

	if (COM_QUERY == command) {
		/* we need some more data after the COM_QUERY */
		if (packet->len < NET_HEADER_SIZE + 2) return PROXY_SEND_QUERY;

		/* LOAD DATA INFILE is nasty */
		if (packet->len - NET_HEADER_SIZE - 1 >= sizeof("LOAD ") - 1 &&
		    0 == g_ascii_strncasecmp(packet->str + NET_HEADER_SIZE + 1, C("LOAD "))) return PROXY_SEND_QUERY;
	}

	/* reset the query status */
	network_injection_queue_reset(st->injected.queries);

	/* ok, here we go */

#ifdef HAVE_LUA_H
	switch(network_mysqld_con_lua_register_callback(con, con->config->lua_script)) {
		case REGISTER_CALLBACK_SUCCESS:
			break;
		case REGISTER_CALLBACK_LOAD_FAILED:
			network_mysqld_con_send_error(con->client, C("MySQL Proxy Lua script failed to load. Check the error log."));
			con->state = CON_STATE_SEND_ERROR;
			return PROXY_SEND_RESULT;
		case REGISTER_CALLBACK_EXECUTE_FAILED:
			network_mysqld_con_send_error(con->client, C("MySQL Proxy Lua script failed to execute. Check the error log."));
			con->state = CON_STATE_SEND_ERROR;
			return PROXY_SEND_RESULT;
	}

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
				 * injection_new(..., query);
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
	} else {
		network_mysqld_con_handle_stmt(NULL, con, packet);
		return PROXY_SEND_RESULT;
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
NETWORK_MYSQLD_PLUGIN_PROTO(server_read_query) {
	GString *packet;
	GList *chunk;
	network_socket *recv_sock, *send_sock;
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	network_mysqld_lua_stmt_ret ret;

	send_sock = NULL;
	recv_sock = con->client;
	st->injected.sent_resultset = 0;

	chunk = recv_sock->recv_queue->chunks->head;

	if (recv_sock->recv_queue->chunks->length != 1) {
		g_message("%s.%d: client-recv-queue-len = %d", __FILE__, __LINE__, recv_sock->recv_queue->chunks->length);
	}
	
	packet = chunk->data;

	ret = master_lua_read_query(con);

	switch (ret) {
	case PROXY_NO_DECISION:
		network_mysqld_con_send_error(con->client, C("need a resultset + proxy.PROXY_SEND_RESULT"));
		con->state = CON_STATE_SEND_ERROR;
		break;
	case PROXY_SEND_RESULT: 
		con->state = CON_STATE_SEND_QUERY_RESULT;
		break; 
	default:
		network_mysqld_con_send_error(con->client, C("need a resultset + proxy.PROXY_SEND_RESULT ... got something else"));

		con->state = CON_STATE_SEND_ERROR;
		break;
	}

	g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

	return NETWORK_SOCKET_SUCCESS;
}

/**
 * cleanup the master specific data on the current connection 
 *
 * @return NETWORK_SOCKET_SUCCESS
 */
NETWORK_MYSQLD_PLUGIN_PROTO(master_disconnect_client) {
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	lua_scope  *sc = con->srv->priv->sc;

	if (st == NULL) return NETWORK_SOCKET_SUCCESS;
	
#ifdef HAVE_LUA_H
	/* remove this cached script from registry */
	if (st->L_ref > 0) {
		luaL_unref(sc->L, LUA_REGISTRYINDEX, st->L_ref);
	}
#endif

	network_mysqld_con_lua_free(st);

	con->plugin_con_state = NULL;

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(master_get_more_rows) {
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	gboolean is_done = FALSE;

	if (st->L) {
		lua_State *L = st->L;

		lua_getglobal(L, "proxy");
		lua_getfield(L, -1, "response");
		lua_getfield(L, -1, "packets");

		if (!lua_isnil(L, -1)) {
			if (0 != lua_pcall(L, 0, 1, 0)) {
				g_critical("%s: %s",
						G_STRLOC,
						lua_tostring(L, -1));
				lua_pop(L, 1);
			} else {
				if (lua_isnil(L, -1)) {
					/* we are done */
					con->state = CON_STATE_READ_QUERY;
				} else if (lua_isstring(L, -1)) {
					size_t str_len;
					const char *str = lua_tolstring(L, -1, &str_len);

					/* stay in this state and send the data */
					network_mysqld_queue_append(con->client, con->client->send_queue, str, str_len);
				} else {
					g_critical("%s: the iterator should either return a string or nil", G_STRLOC);
					con->state = CON_STATE_ERROR;
				}
				lua_pop(L, 1);
			}
		} else {
			con->state = CON_STATE_READ_QUERY;
			lua_pop(L, 1);
		}

		lua_pop(L, 2);
	} else {
		con->state = CON_STATE_READ_QUERY;
	}

	return NETWORK_SOCKET_SUCCESS;
}

static int network_mysqld_server_connection_init(network_mysqld_con *con) {
	con->plugins.con_init             = server_con_init;

	con->plugins.con_read_auth        = server_read_auth;

	con->plugins.con_read_query       = server_read_query;
	con->plugins.con_read_query_result = master_get_more_rows;
	con->plugins.con_send_query_result = master_get_more_rows; /* call the resultset iterator to get more data to send */
	
	con->plugins.con_cleanup          = master_disconnect_client;

	return 0;
}

static chassis_plugin_config *network_mysqld_master_plugin_new(void) {
	chassis_plugin_config *config;

	config = g_new0(chassis_plugin_config, 1);

	return config;
}

static void network_mysqld_master_plugin_free(chassis_plugin_config *config) {
	if (config->listen_con) {
		/* the socket will be freed by network_mysqld_free() */
	}

	if (config->address) {
		g_free(config->address);
	}

	if (config->master_username) g_free(config->master_username);
	if (config->master_password) g_free(config->master_password);
	if (config->lua_script) g_free(config->lua_script);

	g_free(config);
}

/**
 * add the proxy specific options to the cmdline interface 
 */
static GOptionEntry * network_mysqld_master_plugin_get_options(chassis_plugin_config *config) {
	guint i;

	static GOptionEntry config_entries[] = 
	{
		{ "master-address",            0, 0, G_OPTION_ARG_STRING, NULL, "listening address:port of the master-server (default: :4041)", "<host:port>" },
		{ "master-username",           0, 0, G_OPTION_ARG_STRING, NULL, "username to allow to log in (default: root)", "<string>" },
		{ "master-password",           0, 0, G_OPTION_ARG_STRING, NULL, "password to allow to log in (default: )", "<string>" },
		{ "master-lua-script",         0, 0, G_OPTION_ARG_FILENAME, NULL, "script to execute by the master plugin", "<filename>" },
		
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

	i = 0;
	config_entries[i++].arg_data = &(config->address);
	config_entries[i++].arg_data = &(config->master_username);
	config_entries[i++].arg_data = &(config->master_password);
	config_entries[i++].arg_data = &(config->lua_script);

	return config_entries;
}

/**
 * init the plugin with the parsed config
 */
static int network_mysqld_master_plugin_apply_config(chassis *chas, chassis_plugin_config *config) {
	network_mysqld_con *con;
	network_socket *listen_sock;

	if (!config->address) config->address = g_strdup(":4041");
	if (!config->master_username) config->master_username = g_strdup("root");
	if (!config->master_password) config->master_password = g_strdup("secret");

	/** 
	 * create a connection handle for the listen socket 
	 */
	con = network_mysqld_con_new();
	network_mysqld_add_connection(chas, con);
	con->config = config;

	config->listen_con = con;
	
	listen_sock = network_socket_new();
	con->server = listen_sock;

	/* set the plugin hooks as we want to apply them to the new connections too later */
	network_mysqld_server_connection_init(con);

	/* FIXME: network_socket_set_address() */
	if (0 != network_address_set_address(listen_sock->dst, config->address)) {
		return -1;
	}

	/* FIXME: network_socket_bind() */
	if (0 != network_socket_bind(listen_sock)) {
		return -1;
	}

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
	p->name         = g_strdup("master");
	p->version		= g_strdup("0.7.0");

	p->init         = network_mysqld_master_plugin_new;
	p->get_options  = network_mysqld_master_plugin_get_options;
	p->apply_config = network_mysqld_master_plugin_apply_config;
	p->destroy      = network_mysqld_master_plugin_free;

	return 0;
}


