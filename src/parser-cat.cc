/*
 * Copyright (C) 2001,2002,2003 Red Hat, Inc.
 * Copyright © 2017, 2018 Christian Persch
 *
 * This programme is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This programme is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <glib.h>
#include <locale.h>
#include <unistd.h>
#include <fcntl.h>

#include <cassert>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "debug.h"
#include "iso2022.h"
#include "parser.hh"

static char const*
seq_to_str(unsigned int type)
{
        switch (type) {
        case VTE_SEQ_NONE: return "NONE";
        case VTE_SEQ_IGNORE: return "IGNORE";
        case VTE_SEQ_GRAPHIC: return "GRAPHIC";
        case VTE_SEQ_CONTROL: return "CONTROL";
        case VTE_SEQ_ESCAPE: return "ESCAPE";
        case VTE_SEQ_CSI: return "CSI";
        case VTE_SEQ_DCS: return "DCS";
        case VTE_SEQ_OSC: return "OSC";
        default:
                assert(false);
        }
}

static char const*
cmd_to_str(unsigned int command)
{
        switch (command) {
#define _VTE_CMD(cmd) case VTE_CMD_##cmd: return #cmd;
#include "parser-cmd.hh"
#undef _VTE_CMD
        default:
                return nullptr;
        }
}

#if 0
static char const*
charset_alias_to_str(unsigned int cs)
{
        switch (cs) {
#define _VTE_CHARSET_PASTE(name)
#define _VTE_CHARSET(name) _VTE_CHARSET_PASTE(name)
#define _VTE_CHARSET_ALIAS_PASTE(name1,name2) case VTE_CHARSET_##name1: return #name1 "(" ## #name2 ## ")";
#define _VTE_CHARSET_ALIAS(name1,name2)
#include "parser-charset.hh"
#undef _VTE_CHARSET_PASTE
#undef _VTE_CHARSET
#undef _VTE_CHARSET_ALIAS_PASTE
#undef _VTE_CHARSET_ALIAS
        default:
                return nullptr; /* not an alias */
        }
}

static char const*
charset_to_str(unsigned int cs)
{
        auto alias = charset_alias_to_str(cs);
        if (alias)
                return alias;

        switch (cs) {
#define _VTE_CHARSET_PASTE(name) case VTE_CHARSET_##name: return #name;
#define _VTE_CHARSET(name) _VTE_CHARSET_PASTE(name)
#define _VTE_CHARSET_ALIAS_PASTE(name1,name2)
#define _VTE_CHARSET_ALIAS(name1,name2)
#include "parser-charset.hh"
#undef _VTE_CHARSET_PASTE
#undef _VTE_CHARSET
#undef _VTE_CHARSET_ALIAS_PASTE
#undef _VTE_CHARSET_ALIAS
        default:
                static char buf[32];
                snprintf(buf, sizeof(buf), "UNKOWN(%u)", cs);
                return buf;
        }
}
#endif

#define SEQ_START "\e[7m"
#define SEQ_END   "\e[27m"

#define SEQ_START_RED "\e[7;31m"
#define SEQ_END_RED   "\e[27;39m"

class printer {
public:
        printer(GString* str,
                bool plain,
                char const* intro,
                char const* outro)
                : m_str(str),
                  m_plain(plain),
                  m_outro(outro) {
                if (!m_plain)
                        g_string_append(m_str, intro);
        }
        ~printer() {
                if (!m_plain)
                        g_string_append(m_str, m_outro);
        }
private:
        GString* m_str;
        bool m_plain;
        char const* m_outro;
};

static bool
print_params(GString* str,
             struct vte_seq const* seq)
{
        for (unsigned int i = 0; i < seq->n_args; i++) {
                auto arg = seq->args[i];
                if (!vte_seq_arg_default(arg))
                        g_string_append_printf(str, "%d", vte_seq_arg_value(arg));
                if (i + 1 < seq->n_args)
                        g_string_append_c(str, vte_seq_arg_nonfinal(arg) ? ':' : ';');
        }

        return seq->n_args > 0;
}

static bool
print_intermediates(GString* str,
                    unsigned int intermediates,
                    unsigned int start,
                    unsigned int end)
{
        bool any = false;
        for (unsigned int i = start; i <= end; i++) {
                unsigned int mask = (1U << (i - 0x20));

                if (intermediates & mask) {
                        g_string_append_c(str, i);
                        any = true;
                }
        }

        return any;
}

static void
print_seq_and_params(GString* str,
                     const struct vte_seq *seq,
                     bool plain)
{
        printer p(str, plain, SEQ_START, SEQ_END);

        if (seq->command != VTE_CMD_NONE) {
                g_string_append_printf(str, "{%s ", cmd_to_str(seq->command));
                print_params(str, seq);
                g_string_append_c(str, '}');
        } else {
                g_string_append_printf(str, "{%s ", seq_to_str(seq->type));
                if ((seq->intermediates & 0xffff0000U) &&
                    print_intermediates(str, seq->intermediates, 0x30, 0x3f))
                        g_string_append_c(str, ' ');
                if (print_params(str, seq))
                        g_string_append_c(str, ' ');
                if ((seq->intermediates & 0x0000ffffU) &&
                    print_intermediates(str, seq->intermediates, 0x20, 0x2f))
                        g_string_append_c(str, ' ');
                g_string_append_printf(str, " %c}", seq->terminator);
        }
}

static void
print_seq(GString* str,
          struct vte_seq const* seq,
          bool codepoints,
          bool plain)
{
        switch (seq->type) {
        case VTE_SEQ_NONE: {
                printer p(str, plain, SEQ_START_RED, SEQ_END_RED);
                g_string_append(str, "{NONE}");
                break;
        }

        case VTE_SEQ_IGNORE: {
                printer p(str, plain, SEQ_START_RED, SEQ_END_RED);
                g_string_append(str, "{IGN}");
                break;
        }

        case VTE_SEQ_GRAPHIC: {
                bool printable = g_unichar_isprint(seq->terminator);
                if (codepoints || !printable) {
                        if (printable) {
                                char ubuf[7];
                                ubuf[g_unichar_to_utf8(seq->terminator, ubuf)] = 0;
                                g_string_append_printf(str, "[%04X %s]",
                                                       seq->terminator, ubuf);
                        } else {
                                g_string_append_printf(str, "[%04X]",
                                                       seq->terminator);
                        }
                } else {
                        g_string_append_unichar(str, seq->terminator);
                }
                break;
        }

        case VTE_SEQ_CONTROL:
        case VTE_SEQ_ESCAPE: {
                printer p(str, plain, SEQ_START, SEQ_END);
                g_string_append_printf(str, "{%s}", cmd_to_str(seq->command));
                break;
        }

        case VTE_SEQ_CSI:
        case VTE_SEQ_DCS:
        case VTE_SEQ_OSC: {
                print_seq_and_params(str, seq, plain);
                break;
        }

        default:
                assert(false);
        }
}

static void
printout(GString* str)
{
        g_print("%s\n", str->str);
        g_string_truncate(str, 0);
}

static gsize seq_stats[VTE_SEQ_N];
static gsize cmd_stats[VTE_CMD_N];
static GArray* bench_times;

static void
process_file(int fd,
             char const* charset,
             bool codepoints,
             bool plain,
             bool quiet)
{
        struct vte_parser *parser;
        if (vte_parser_new(&parser) != 0)
                return;

        auto subst = _vte_iso2022_state_new(charset);

        gsize const buf_size = 16384;
        guchar* buf = g_new0(guchar, buf_size);
        auto unichars = g_array_new(FALSE, FALSE, sizeof(gunichar));
        auto outbuf = g_string_sized_new(buf_size);

        auto start_time = g_get_monotonic_time();

        gsize buf_start = 0;
        for (;;) {
                auto len = read(fd, buf + buf_start, buf_size - buf_start);
                if (!len)
                        break;
                if (len == -1) {
                        if (errno == EAGAIN)
                                continue;
                        break;
                }

                g_array_set_size(unichars, 0);
                auto plen = _vte_iso2022_process(subst, buf, len, unichars);
                if ((gsize)plen != (gsize)len) {
                        /* Save it for next round */
                        memmove(buf, buf + plen, len - plen);
                        buf_start = len - plen;
                } else
                        buf_start = 0;

                auto wbuf = &g_array_index(unichars, gunichar, 0);
                gsize wcount = unichars->len;

                struct vte_seq *seq;
                for (gsize i = 0; i < wcount; i++) {
                        auto ret = vte_parser_feed(parser,
                                                   &seq,
                                                   wbuf[i]);
                        if (G_UNLIKELY(ret < 0)) {
                                g_printerr("Parser error!\n");
                                goto out;
                        }

                        seq_stats[ret]++;
                        if (ret != VTE_SEQ_NONE) {
                                cmd_stats[seq->command]++;
                                if (!quiet) {
                                        print_seq(outbuf, seq, codepoints, plain);
                                        if (seq->command == VTE_CMD_LF)
                                                printout(outbuf);
                                }
                        }
                }
        }

 out:
        if (!quiet)
                printout(outbuf);

        int64_t time_spent = g_get_monotonic_time() - start_time;
        g_array_append_val(bench_times, time_spent);

        g_string_free(outbuf, TRUE);
        g_array_free(unichars, TRUE);
        g_free(buf);
        vte_parser_free(parser);
        _vte_iso2022_state_free(subst);
}

static bool
process_file(int fd,
             char const* charset,
             bool codepoints,
             bool plain,
             bool quiet,
             int repeat)
{
        if (fd == STDIN_FILENO && repeat != 1) {
                g_printerr("Cannot consume STDIN more than once\n");
                return false;
        }

        for (auto i = 0; i < repeat; ++i) {
                if (i > 0 && lseek(fd, 0, SEEK_SET) != 0) {
                        g_printerr("Failed to seek: %m\n");
                        return false;
                }

                process_file(fd, charset, codepoints, plain, quiet);
        }

        return true;
}

int
main(int argc,
     char *argv[])
{
        gboolean benchmark = false;
        gboolean codepoints = false;
        gboolean plain = false;
        gboolean quiet = false;
        gboolean statistics = false;
        int repeat = 1;
        char* charset = nullptr;
        char** filenames = nullptr;
        GOptionEntry const entries[] = {
                { "benchmark", 'b', 0, G_OPTION_ARG_NONE, &benchmark,
                  "Measure time spent parsing each file", nullptr },
                { "charset", 'c', 0, G_OPTION_ARG_STRING, &charset,
                  "Charset to use (default: UTF-8)", "CHARSET" },
                { "codepoints", 'u', 0, G_OPTION_ARG_NONE, &codepoints,
                  "Output unicode code points by number", nullptr },
                { "plain", 'p', 0, G_OPTION_ARG_NONE, &plain,
                  "Output plain text without attributes", nullptr },
                { "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet,
                  "Suppress output except for statistics and benchmark", nullptr },
                { "repeat", 'r', 0, G_OPTION_ARG_INT, &repeat,
                  "Repeat each file COUNT times", "COUNT" },
                { "statistics", 's', 0, G_OPTION_ARG_NONE, &statistics,
                  "Output statistics", nullptr },
                { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &filenames,
                  nullptr, nullptr },
                { nullptr }
        };

        setlocale(LC_ALL, "");
        _vte_debug_init();

        auto context = g_option_context_new("[FILE…] — parser cat");
        g_option_context_set_help_enabled(context, true);
        g_option_context_add_main_entries(context, entries, nullptr);

        GError* err = nullptr;
        bool rv = g_option_context_parse(context, &argc, &argv, &err);
        g_option_context_free(context);

        if (!rv) {
                g_printerr("Failed to parse arguments: %s\n", err->message);
                g_error_free(err);
                return EXIT_FAILURE;
        }

        int exit_status = EXIT_FAILURE;

        memset(&seq_stats, 0, sizeof(seq_stats));
        memset(&cmd_stats, 0, sizeof(cmd_stats));
        bench_times = g_array_new(false, true, sizeof(int64_t));

        if (filenames != nullptr) {
                for (auto i = 0; filenames[i] != nullptr; i++) {
                        char const* filename = filenames[i];

                        int fd = -1;
                        if (g_str_equal(filename, "-")) {
                                fd = STDIN_FILENO;
                        } else {
                                fd = open(filename, O_RDONLY);
                                if (fd == -1) {
                                        g_printerr("Error opening file %s: %m\n", filename);
                                }
                        }
                        if (fd != -1) {
                                bool r = process_file(fd, charset, codepoints, plain, quiet, repeat);
                                close(fd);
                                if (!r)
                                        break;
                        }
                }

                g_strfreev(filenames);
                exit_status = EXIT_SUCCESS;
        } else {
                if (process_file(STDIN_FILENO, charset, codepoints, plain, quiet, repeat))
                        exit_status = EXIT_SUCCESS;
        }

        g_free(charset);

        if (statistics) {
                for (unsigned int s = VTE_SEQ_NONE + 1; s < VTE_SEQ_N; s++) {
                        g_printerr("%-7s: %" G_GSIZE_FORMAT "\n", seq_to_str(s), seq_stats[s]);
                }

                g_printerr("\n");
                for (unsigned int s = 0; s < VTE_CMD_N; s++) {
                        if (cmd_stats[s] > 0) {
                                g_printerr("%-12s: %" G_GSIZE_FORMAT "\n", cmd_to_str(s), cmd_stats[s]);
                        }
                }
        }

        if (benchmark) {
                g_array_sort(bench_times,
                             [](void const* p1, void const* p2) -> int {
                                     int64_t const t1 = *(int64_t const*)p1;
                                     int64_t const t2 = *(int64_t const*)p2;
                                     return t1 == t2 ? 0 : (t1 < t2 ? -1 : 1);
                             });

                int64_t total_time = 0;
                for (unsigned int i = 0; i < bench_times->len; ++i)
                        total_time += g_array_index(bench_times, int64_t, i);

                g_printerr("\nTimes: best %\'" G_GINT64_FORMAT "µs "
                           "worst %\'" G_GINT64_FORMAT "µs "
                           "average %\'" G_GINT64_FORMAT "µs\n",
                           g_array_index(bench_times, int64_t, 0),
                           g_array_index(bench_times, int64_t, bench_times->len - 1),
                           total_time / (int64_t)bench_times->len);
                for (unsigned int i = 0; i < bench_times->len; ++i)
                        g_printerr("  %\'" G_GINT64_FORMAT "µs\n",
                                   g_array_index(bench_times, int64_t, i));
        }

        g_array_free(bench_times,true);

        return exit_status;
}