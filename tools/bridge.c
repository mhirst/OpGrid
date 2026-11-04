/*
 * Line-protocol host for the OpGrid plugin, so the JS UI can run against
 * the real engine on a desktop (screenshots, end-to-end tests).
 *   S <key>\t<value>   -> set_param, replies "."
 *   G <key>            -> get_param, replies the value ("\x01" = no value)
 *   R <blocks>         -> render that many 128-frame blocks, replies "."
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api_v1.h"

extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

static float bpm(void) { return 120.0f; }

int main(void) {
    static host_api_v1_t host;
    host.sample_rate = 44100;
    host.frames_per_block = 128;
    host.get_bpm = bpm;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *in = api->create_instance(".", NULL);
    static char line[1 << 18], buf[1 << 18];
    int16_t audio[256];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = 0;
        if (line[0] == 'S' && line[1] == ' ') {
            char *tab = strchr(line + 2, '\t');
            if (tab) { *tab = 0; api->set_param(in, line + 2, tab + 1); }
            fputs(".\n", stdout);
        } else if (line[0] == 'G' && line[1] == ' ') {
            int r = api->get_param(in, line + 2, buf, sizeof(buf));
            if (r < 0) fputs("\x01\n", stdout); else { fputs(buf, stdout); fputc('\n', stdout); }
        } else if (line[0] == 'R' && line[1] == ' ') {
            int blocks = atoi(line + 2);
            for (int i = 0; i < blocks; i++) api->render_block(in, audio, 128);
            fputs(".\n", stdout);
        } else {
            fputs("?\n", stdout);
        }
        fflush(stdout);
    }
    api->destroy_instance(in);
    return 0;
}
