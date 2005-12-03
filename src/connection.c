/*
 * uoproxy
 * $Id$
 *
 * (c) 2005 Max Kellermann <max@duempel.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "connection.h"
#include "server.h"
#include "client.h"
#include "relay.h"
#include "handler.h"

int connection_new(int server_socket,
                   u_int32_t local_ip, u_int16_t local_port,
                   u_int32_t server_ip, u_int16_t server_port,
                   struct connection **connectionp) {
    struct connection *c;
    int ret;

    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return -ENOMEM;

    c->local_ip = local_ip;
    c->local_port = local_port;
    ret = uo_server_create(server_socket, &c->server);
    if (ret != 0) {
        connection_delete(c);
        return ret;
    }

    c->server_ip = server_ip;
    c->server_port = server_port;

    *connectionp = c;

    return 0;
}

void connection_delete(struct connection *c) {
    if (c->server != NULL)
        uo_server_dispose(c->server);
    if (c->client != NULL)
        uo_client_dispose(c->client);
    free(c);
}

void connection_invalidate(struct connection *c) {
    c->invalid = 1;

    if (c->server != NULL) {
        uo_server_dispose(c->server);
        c->server = NULL;
    }

    if (c->client != NULL) {
        uo_client_dispose(c->client);
        c->client = NULL;
    }
}

void connection_pre_select(struct connection *c, struct selectx *sx) {
    if (c->invalid)
        return;

    if (c->client != NULL &&
        !uo_client_alive(c->client)) {
        printf("server disconnected\n");
        connection_invalidate(c);
        return;
    }

    if (c->server != NULL &&
        !uo_server_alive(c->server)) {
        printf("client disconnected\n");
        connection_invalidate(c);
        return;
    }

    if (c->client != NULL)
        uo_client_pre_select(c->client, sx);

    if (c->server != NULL)
        uo_server_pre_select(c->server, sx);
}

int connection_post_select(struct connection *c, struct selectx *sx) {
    void *p;
    size_t length;
    packet_action_t action;

    if (c->invalid)
        return 0;

    if (c->client != NULL) {
        uo_client_post_select(c->client, sx);
        while (c->client != NULL &&
               (p = uo_client_peek(c->client, &length)) != NULL) {
            action = handle_packet(server_packet_bindings,
                                   c, p, length);
            switch (action) {
            case PA_ACCEPT:
                if (c->server != NULL)
                    uo_server_send(c->server, p, length);
                break;

            case PA_DROP:
                break;

            case PA_DISCONNECT:
                connection_invalidate(c);
                break;
            }

            if (c->client != NULL)
                uo_client_shift(c->client, length);
        }
    }

    if (c->server != NULL) {
        uo_server_post_select(c->server, sx);
        while (c->server != NULL &&
               (p = uo_server_peek(c->server, &length)) != NULL) {
            action = handle_packet(client_packet_bindings,
                                   c, p, length);
            switch (action) {
            case PA_ACCEPT:
                if (c->client != NULL)
                    uo_client_send(c->client, p, length);
                break;

            case PA_DROP:
                break;

            case PA_DISCONNECT:
                connection_invalidate(c);
                break;
            }

            if (c->server != NULL)
                uo_server_shift(c->server, length);
        }
    }

    return 0;
}