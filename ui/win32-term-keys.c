/*
 * Key translation for the PuTTY-terminal-backed text console
 *
 * SPDX-License-Identifier: MIT
 *
 * DERIVED FROM THIRD-PARTY CODE.  The body of TranslateKey() below is
 * PuTTY 0.85's windows/window.c TranslateKey(), copied with the smallest
 * possible set of changes.  PuTTY is copyright 1997-2026 Simon Tatham and
 * others and is distributed under the MIT licence; the full copyright
 * notice and permission text are in ui/putty/LICENCE, which applies to
 * this file as well.  It is kept outside ui/putty/ precisely because it is
 * *not* verbatim, and everything in that directory is.
 *
 * This is 700 lines of accumulated knowledge about how a Windows keyboard
 * maps onto a VT220/xterm keypad -- application cursor keys, the three
 * different function-key encodings, AltGr, dead keys, the numeric keypad
 * under NumLock, Alt-numpad Unicode entry and the compose key.  Rewriting
 * it would have meant reinventing all of that, badly.
 *
 * The changes made, and nothing else:
 *
 *   - 'WinGuiSeat *wgs' becomes 'Win32Term *wgs'.  The field names the
 *     function uses (conf, term, term_hwnd, ucsdata, compose_state,
 *     compose_keycode, compose_char, alt_numberpad_accumulator) are
 *     deliberately spelled the same way in struct Win32Term, so the body
 *     is untouched.
 *   - show_mouseptr() becomes win32_term_show_mouseptr(), ours.
 *   - Ctrl-Break sent a PuTTY backend special; it now goes through the
 *     break callback, which the chardev turns into a real serial BREAK.
 *   - clips_system[], a file-static in window.c, is declared here.
 */

#include "win32-term-internal.h"

static const int clips_system[] = { CLIP_SYSTEM };

int win32_term_translate_key(Win32Term *wgs, UINT message, WPARAM wParam,
                        LPARAM lParam, unsigned char *output)
{
    BYTE keystate[256];
    int scan, shift_state;
    bool left_alt = false, key_down;
    int r, i;
    unsigned char *p = output;
    int funky_type = conf_get_int(wgs->conf, CONF_funky_type);
    bool no_applic_k = conf_get_bool(wgs->conf, CONF_no_applic_k);
    bool ctrlaltkeys = conf_get_bool(wgs->conf, CONF_ctrlaltkeys);
    bool nethack_keypad = conf_get_bool(wgs->conf, CONF_nethack_keypad);
    char keypad_key = '\0';

    HKL kbd_layout = GetKeyboardLayout(0);

    r = GetKeyboardState(keystate);
    if (!r)
        memset(keystate, 0, sizeof(keystate));
    else {
#if 0
#define SHOW_TOASCII_RESULT
        {                              /* Tell us all about key events */
            static BYTE oldstate[256];
            static int first = 1;
            static int scan;
            int ch;
            if (first)
                memcpy(oldstate, keystate, sizeof(oldstate));
            first = 0;

            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT) {
                debug("+");
            } else if ((HIWORD(lParam) & KF_UP)
                       && scan == (HIWORD(lParam) & 0xFF)) {
                debug(". U");
            } else {
                debug(".\n");
                if (wParam >= VK_F1 && wParam <= VK_F20)
                    debug("K_F%d", wParam + 1 - VK_F1);
                else
                    switch (wParam) {
                      case VK_SHIFT:
                        debug("SHIFT");
                        break;
                      case VK_CONTROL:
                        debug("CTRL");
                        break;
                      case VK_MENU:
                        debug("ALT");
                        break;
                      default:
                        debug("VK_%02x", wParam);
                    }
                if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
                    debug("*");
                debug(", S%02x", scan = (HIWORD(lParam) & 0xFF));

                ch = MapVirtualKeyEx(wParam, 2, kbd_layout);
                if (ch >= ' ' && ch <= '~')
                    debug(", '%c'", ch);
                else if (ch)
                    debug(", $%02x", ch);

                if ((keystate[VK_SHIFT] & 0x80) != 0)
                    debug(", S");
                if ((keystate[VK_CONTROL] & 0x80) != 0)
                    debug(", C");
                if ((HIWORD(lParam) & KF_EXTENDED))
                    debug(", E");
                if ((HIWORD(lParam) & KF_UP))
                    debug(", U");
            }

            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT);
            else if ((HIWORD(lParam) & KF_UP))
                oldstate[wParam & 0xFF] ^= 0x80;
            else
                oldstate[wParam & 0xFF] ^= 0x81;

            for (ch = 0; ch < 256; ch++)
                if (oldstate[ch] != keystate[ch])
                    debug(", M%02x=%02x", ch, keystate[ch]);

            memcpy(oldstate, keystate, sizeof(oldstate));
        }
#endif

        if (wParam == VK_MENU && (HIWORD(lParam) & KF_EXTENDED)) {
            keystate[VK_RMENU] = keystate[VK_MENU];
        }


        /* Nastiness with NUMLock - Shift-NUMLock is left alone though */
        if ((funky_type == FUNKY_VT400 ||
             (funky_type <= FUNKY_LINUX && wgs->term->app_keypad_keys &&
              !no_applic_k))
            && wParam == VK_NUMLOCK && !(keystate[VK_SHIFT] & 0x80)) {

            wParam = VK_EXECUTE;

            /* UnToggle NUMLock */
            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == 0)
                keystate[VK_NUMLOCK] ^= 1;
        }

        /* And write back the 'adjusted' state */
        SetKeyboardState(keystate);
    }

    /* Disable Auto repeat if required */
    if (wgs->term->repeat_off &&
        (HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT)
        return 0;

    if ((HIWORD(lParam) & KF_ALTDOWN) && (keystate[VK_RMENU] & 0x80) == 0)
        left_alt = true;

    key_down = ((HIWORD(lParam) & KF_UP) == 0);

    /* Make sure Ctrl-ALT is not the same as AltGr for ToAscii unless told. */
    if (left_alt && (keystate[VK_CONTROL] & 0x80)) {
        if (ctrlaltkeys)
            keystate[VK_MENU] = 0;
        else {
            keystate[VK_RMENU] = 0x80;
            left_alt = false;
        }
    }

    scan = (HIWORD(lParam) & (KF_UP | KF_EXTENDED | 0xFF));
    shift_state = ((keystate[VK_SHIFT] & 0x80) != 0)
        + ((keystate[VK_CONTROL] & 0x80) != 0) * 2;

    /* Note if AltGr was pressed and if it was used as a compose key */
    if (!wgs->compose_state) {
        wgs->compose_keycode = 0x100;
        if (conf_get_bool(wgs->conf, CONF_compose_key)) {
            if (wParam == VK_MENU && (HIWORD(lParam) & KF_EXTENDED))
                wgs->compose_keycode = wParam;
        }
        if (wParam == VK_APPS)
            wgs->compose_keycode = wParam;
    }

    if (wParam == wgs->compose_keycode) {
        if (wgs->compose_state == 0 &&
            (HIWORD(lParam) & (KF_UP | KF_REPEAT)) == 0)
            wgs->compose_state = 1;
        else if (wgs->compose_state == 1 && (HIWORD(lParam) & KF_UP))
            wgs->compose_state = 2;
        else
            wgs->compose_state = 0;
    } else if (wgs->compose_state == 1 && wParam != VK_CONTROL)
        wgs->compose_state = 0;

    if (wgs->compose_state > 1 && left_alt)
        wgs->compose_state = 0;

    /* Sanitize the number pad if not using a PC NumPad */
    if (left_alt || (wgs->term->app_keypad_keys && !no_applic_k
                     && funky_type != FUNKY_XTERM) ||
        funky_type == FUNKY_VT400 || nethack_keypad || wgs->compose_state) {
        if ((HIWORD(lParam) & KF_EXTENDED) == 0) {
            int nParam = 0;
            switch (wParam) {
              case VK_INSERT:
                nParam = VK_NUMPAD0;
                break;
              case VK_END:
                nParam = VK_NUMPAD1;
                break;
              case VK_DOWN:
                nParam = VK_NUMPAD2;
                break;
              case VK_NEXT:
                nParam = VK_NUMPAD3;
                break;
              case VK_LEFT:
                nParam = VK_NUMPAD4;
                break;
              case VK_CLEAR:
                nParam = VK_NUMPAD5;
                break;
              case VK_RIGHT:
                nParam = VK_NUMPAD6;
                break;
              case VK_HOME:
                nParam = VK_NUMPAD7;
                break;
              case VK_UP:
                nParam = VK_NUMPAD8;
                break;
              case VK_PRIOR:
                nParam = VK_NUMPAD9;
                break;
              case VK_DELETE:
                nParam = VK_DECIMAL;
                break;
            }
            if (nParam) {
                if (keystate[VK_NUMLOCK] & 1)
                    shift_state |= 1;
                wParam = nParam;
            }
        }
    }

    /* If a key is pressed and AltGr is not active */
    if (key_down && (keystate[VK_RMENU] & 0x80) == 0 && !wgs->compose_state) {
        /* Okay, prepare for most alts then ... */
        if (left_alt)
            *p++ = '\033';

        /* Lets see if it's a pattern we know all about ... */
        if (wParam == VK_PRIOR && shift_state == 1) {
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_PAGEUP, 0);
            return 0;
        }
        if (wParam == VK_PRIOR && shift_state == 3) { /* ctrl-shift-pageup */
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_TOP, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 3) { /* ctrl-shift-pagedown */
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_BOTTOM, 0);
            return 0;
        }

        if (wParam == VK_PRIOR && shift_state == 2) {
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_LINEUP, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 1) {
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_PAGEDOWN, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 2) {
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
            return 0;
        }
        if ((wParam == VK_PRIOR || wParam == VK_NEXT) && shift_state == 3) {
            term_scroll_to_selection(wgs->term, (wParam == VK_PRIOR ? 0 : 1));
            return 0;
        }
        if (wParam == VK_INSERT && shift_state == 2) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftins)) {
              case CLIPUI_IMPLICIT:
                break;          /* no need to re-copy to CLIP_LOCAL */
              case CLIPUI_EXPLICIT:
                term_request_copy(wgs->term, clips_system,
                                  lenof(clips_system));
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == VK_INSERT && shift_state == 1) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftins)) {
              case CLIPUI_IMPLICIT:
                term_request_paste(wgs->term, CLIP_LOCAL);
                break;
              case CLIPUI_EXPLICIT:
                term_request_paste(wgs->term, CLIP_SYSTEM);
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == 'C' && shift_state == 3) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftcv)) {
              case CLIPUI_IMPLICIT:
                break;          /* no need to re-copy to CLIP_LOCAL */
              case CLIPUI_EXPLICIT:
                term_request_copy(wgs->term, clips_system,
                                  lenof(clips_system));
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == 'V' && shift_state == 3) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftcv)) {
              case CLIPUI_IMPLICIT:
                term_request_paste(wgs->term, CLIP_LOCAL);
                break;
              case CLIPUI_EXPLICIT:
                term_request_paste(wgs->term, CLIP_SYSTEM);
                break;
              default:
                break;
            }
            return 0;
        }
        if (left_alt && wParam == VK_F4 &&
            conf_get_bool(wgs->conf, CONF_alt_f4)) {
            return -1;
        }
        if (left_alt && wParam == VK_SPACE &&
            conf_get_bool(wgs->conf, CONF_alt_space)) {
            SendMessage(wgs->term_hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
            return -1;
        }
        if (left_alt && wParam == VK_RETURN &&
            conf_get_bool(wgs->conf, CONF_fullscreenonaltenter) &&
            (conf_get_int(wgs->conf, CONF_resize_action) != RESIZE_DISABLED)) {
            /*
             * Changed: full screen belongs to the frame window, not to a
             * single tab, so let Alt-Enter fall through to it.
             */
            return -1;
        }
        /* Control-Numlock for app-keypad mode switch */
        if (wParam == VK_PAUSE && shift_state == 2) {
            wgs->term->app_keypad_keys = !wgs->term->app_keypad_keys;
            return 0;
        }

        if (wParam == VK_BACK && shift_state == 0) {    /* Backspace */
            *p++ = (conf_get_bool(wgs->conf, CONF_bksp_is_delete) ?
                    0x7F : 0x08);
            *p++ = 0;
            return -2;
        }
        if (wParam == VK_BACK && shift_state == 1) {    /* Shift Backspace */
            /* We do the opposite of what is configured */
            *p++ = (conf_get_bool(wgs->conf, CONF_bksp_is_delete) ?
                    0x08 : 0x7F);
            *p++ = 0;
            return -2;
        }
        if (wParam == VK_TAB && shift_state == 1) {     /* Shift tab */
            *p++ = 0x1B;
            *p++ = '[';
            *p++ = 'Z';
            return p - output;
        }
        if (wParam == VK_SPACE && shift_state == 2) {   /* Ctrl-Space */
            *p++ = 0;
            return p - output;
        }
        if (wParam == VK_SPACE && shift_state == 3) {   /* Ctrl-Shift-Space */
            *p++ = 160;
            return p - output;
        }
        if (wParam == VK_CANCEL && shift_state == 2) {  /* Ctrl-Break */
            win32_term_send_break(wgs);
            return 0;
        }
        if (wParam == VK_PAUSE) {      /* Break/Pause */
            *p++ = 26;
            *p++ = 0;
            return -2;
        }
        /* Control-2 to Control-8 are special */
        if (shift_state == 2 && wParam >= '2' && wParam <= '8') {
            *p++ = "\000\033\034\035\036\037\177"[wParam - '2'];
            return p - output;
        }
        if (shift_state == 2 && (wParam == 0xBD || wParam == 0xBF)) {
            *p++ = 0x1F;
            return p - output;
        }
        if (shift_state == 2 && (wParam == 0xDF || wParam == 0xDC)) {
            *p++ = 0x1C;
            return p - output;
        }
        if (shift_state == 3 && wParam == 0xDE) {
            *p++ = 0x1E;               /* Ctrl-~ == Ctrl-^ in xterm at least */
            return p - output;
        }

        switch (wParam) {
            bool consumed_alt;

          case VK_NUMPAD0: keypad_key = '0'; goto numeric_keypad;
          case VK_NUMPAD1: keypad_key = '1'; goto numeric_keypad;
          case VK_NUMPAD2: keypad_key = '2'; goto numeric_keypad;
          case VK_NUMPAD3: keypad_key = '3'; goto numeric_keypad;
          case VK_NUMPAD4: keypad_key = '4'; goto numeric_keypad;
          case VK_NUMPAD5: keypad_key = '5'; goto numeric_keypad;
          case VK_NUMPAD6: keypad_key = '6'; goto numeric_keypad;
          case VK_NUMPAD7: keypad_key = '7'; goto numeric_keypad;
          case VK_NUMPAD8: keypad_key = '8'; goto numeric_keypad;
          case VK_NUMPAD9: keypad_key = '9'; goto numeric_keypad;
          case VK_DECIMAL: keypad_key = '.'; goto numeric_keypad;
          case VK_ADD: keypad_key = '+'; goto numeric_keypad;
          case VK_SUBTRACT: keypad_key = '-'; goto numeric_keypad;
          case VK_MULTIPLY: keypad_key = '*'; goto numeric_keypad;
          case VK_DIVIDE: keypad_key = '/'; goto numeric_keypad;
          case VK_EXECUTE: keypad_key = 'G'; goto numeric_keypad;
            /* also the case for VK_RETURN below can sometimes come here */
          numeric_keypad:
            /* Left Alt overrides all numeric keypad usage to act as
             * numeric character code input */
            if (left_alt) {
                if (keypad_key >= '0' && keypad_key <= '9')
                    wgs->alt_numberpad_accumulator =
                        wgs->alt_numberpad_accumulator * 10 + keypad_key - '0';
                else
                    wgs->alt_numberpad_accumulator = 0;
                break;
            }

            {
                int nchars = format_numeric_keypad_key(
                    (char *)p, wgs->term, keypad_key,
                    shift_state & 1, shift_state & 2);
                if (!nchars) {
                    /*
                     * If we didn't get an escape sequence out of the
                     * numeric keypad key, then that must be because
                     * we're in Num Lock mode without application
                     * keypad enabled. In that situation we leave this
                     * keypress to the ToUnicode/ToAsciiEx handler
                     * below, which will translate it according to the
                     * appropriate keypad layout (e.g. so that what a
                     * Brit thinks of as keypad '.' can become ',' in
                     * the German layout).
                     *
                     * An exception is the keypad Return key: if we
                     * didn't get an escape sequence for that, we
                     * treat it like ordinary Return, taking into
                     * account Telnet special new line codes and
                     * config options.
                     */
                    if (keypad_key == '\r')
                        goto ordinary_return_key;
                    break;
                }

                p += nchars;
                return p - output;
            }

            int fkey_number;
          case VK_F1: fkey_number = 1; goto numbered_function_key;
          case VK_F2: fkey_number = 2; goto numbered_function_key;
          case VK_F3: fkey_number = 3; goto numbered_function_key;
          case VK_F4: fkey_number = 4; goto numbered_function_key;
          case VK_F5: fkey_number = 5; goto numbered_function_key;
          case VK_F6: fkey_number = 6; goto numbered_function_key;
          case VK_F7: fkey_number = 7; goto numbered_function_key;
          case VK_F8: fkey_number = 8; goto numbered_function_key;
          case VK_F9: fkey_number = 9; goto numbered_function_key;
          case VK_F10: fkey_number = 10; goto numbered_function_key;
          case VK_F11: fkey_number = 11; goto numbered_function_key;
          case VK_F12: fkey_number = 12; goto numbered_function_key;
          case VK_F13: fkey_number = 13; goto numbered_function_key;
          case VK_F14: fkey_number = 14; goto numbered_function_key;
          case VK_F15: fkey_number = 15; goto numbered_function_key;
          case VK_F16: fkey_number = 16; goto numbered_function_key;
          case VK_F17: fkey_number = 17; goto numbered_function_key;
          case VK_F18: fkey_number = 18; goto numbered_function_key;
          case VK_F19: fkey_number = 19; goto numbered_function_key;
          case VK_F20: fkey_number = 20; goto numbered_function_key;
          numbered_function_key:
            consumed_alt = false;
            p += format_function_key((char *)p, wgs->term, fkey_number,
                                     shift_state & 1, shift_state & 2,
                                     left_alt, &consumed_alt);
            if (consumed_alt) {
                /* supersedes the usual prefixing of Esc */
                p -= 1;
                memmove(output, output + 1, p - output);
            }
            return p - output;

            SmallKeypadKey sk_key;
          case VK_HOME: sk_key = SKK_HOME; goto small_keypad_key;
          case VK_END: sk_key = SKK_END; goto small_keypad_key;
          case VK_INSERT: sk_key = SKK_INSERT; goto small_keypad_key;
          case VK_DELETE: sk_key = SKK_DELETE; goto small_keypad_key;
          case VK_PRIOR: sk_key = SKK_PGUP; goto small_keypad_key;
          case VK_NEXT: sk_key = SKK_PGDN; goto small_keypad_key;
          small_keypad_key:
            /* These keys don't generate terminal input with Ctrl */
            if (shift_state & 2)
                break;

            consumed_alt = false;
            p += format_small_keypad_key((char *)p, wgs->term, sk_key,
                                         shift_state & 1, shift_state & 2,
                                         left_alt, &consumed_alt);
            if (consumed_alt) {
                /* supersedes the usual prefixing of Esc */
                p -= 1;
                memmove(output, output + 1, p - output);
            }
            return p - output;

            char xkey;
          case VK_UP: xkey = 'A'; goto arrow_key;
          case VK_DOWN: xkey = 'B'; goto arrow_key;
          case VK_RIGHT: xkey = 'C'; goto arrow_key;
          case VK_LEFT: xkey = 'D'; goto arrow_key;
          case VK_CLEAR: xkey = 'G'; goto arrow_key; /* close enough */
          arrow_key:
            consumed_alt = false;
            p += format_arrow_key((char *)p, wgs->term, xkey, shift_state & 1,
                                  shift_state & 2, left_alt, &consumed_alt);
            if (consumed_alt) {
                /* supersedes the usual prefixing of Esc */
                p -= 1;
                memmove(output, output + 1, p - output);
            }
            return p - output;

          case VK_RETURN:
            if (HIWORD(lParam) & KF_EXTENDED) {
                keypad_key = '\r';
                goto numeric_keypad;
            }
          ordinary_return_key:
            if (shift_state == 0 && wgs->term->cr_lf_return) {
                *p++ = '\r';
                *p++ = '\n';
                return p - output;
            } else {
                *p++ = 0x0D;
                *p++ = 0;
                return -2;
            }
        }
    }

    /* Okay we've done everything interesting; let windows deal with
     * the boring stuff */
    {
        bool capsOn = false;

        /* helg: clear CAPS LOCK state if caps lock switches to cyrillic */
        if (keystate[VK_CAPITAL] != 0 &&
            conf_get_bool(wgs->conf, CONF_xlat_capslockcyr)) {
            capsOn = !left_alt;
            keystate[VK_CAPITAL] = 0;
        }

        wchar_t keys_unicode[3];

        /* XXX how do we know what the max size of the keys array should
         * be is? There's indication on MS' website of an Inquire/InquireEx
         * functioning returning a KBINFO structure which tells us. */
        /*
         * Changed: PuTTY loads ToUnicodeEx() at run time because it still
         * supports Windows 9x, where it does not exist.  We do not, so
         * call it directly.  The 'else' branch below is therefore dead,
         * but is kept so this function stays diffable against upstream.
         */
        if (1) {
            r = ToUnicodeEx(wParam, scan, keystate, keys_unicode,
                            lenof(keys_unicode), 0, kbd_layout);
        } else {
            /* XXX 'keys' parameter is declared in MSDN documentation as
             * 'LPWORD lpChar'.
             * The experience of a French user indicates that on
             * Win98, WORD[] should be passed in, but on Win2K, it should
             * be BYTE[]. German WinXP and my Win2K with "US International"
             * driver corroborate this.
             * Experimentally I've conditionalised the behaviour on the
             * Win9x/NT split, but I suspect it's worse than that.
             * See wishlist item `win-dead-keys' for more horrible detail
             * and speculations. */
            int i;
            WORD keys[3];
            BYTE keysb[3];
            r = ToAsciiEx(wParam, scan, keystate, keys, 0, kbd_layout);
            if (r > 0) {
                for (i = 0; i < r; i++) {
                    keysb[i] = (BYTE)keys[i];
                }
                MultiByteToWideChar(CP_ACP, 0, (LPCSTR)keysb, r,
                                    keys_unicode, lenof(keys_unicode));
            }
        }
#ifdef SHOW_TOASCII_RESULT
        if (r == 1 && !key_down) {
            if (wgs->alt_numberpad_accumulator) {
                if (in_utf(term) || ucsdata.dbcs_screenfont)
                    debug(", (U+%04x)", wgs->alt_numberpad_accumulator);
                else
                    debug(", LCH(%d)", wgs->alt_numberpad_accumulator);
            } else {
                debug(", ACH(%d)", keys_unicode[0]);
            }
        } else if (r > 0) {
            int r1;
            debug(", ASC(");
            for (r1 = 0; r1 < r; r1++) {
                debug("%s%d", r1 ? "," : "", keys_unicode[r1]);
            }
            debug(")");
        }
#endif
        if (r > 0) {
            WCHAR keybuf;

            p = output;
            for (i = 0; i < r; i++) {
                wchar_t wch = keys_unicode[i];

                if (wgs->compose_state == 2 && wch >= ' ' && wch < 0x80) {
                    wgs->compose_char = wch;
                    wgs->compose_state++;
                    continue;
                }
                if (wgs->compose_state == 3 && wch >= ' ' && wch < 0x80) {
                    int nc;
                    wgs->compose_state = 0;

                    if ((nc = check_compose(wgs->compose_char, wch)) == -1) {
                        MessageBeep(MB_ICONHAND);
                        return 0;
                    }
                    keybuf = nc;
                    term_keyinputw(wgs->term, &keybuf, 1);
                    continue;
                }

                wgs->compose_state = 0;

                if (!key_down) {
                    if (wgs->alt_numberpad_accumulator) {
                        if (in_utf(wgs->term) ||
                            wgs->ucsdata.dbcs_screenfont) {
                            keybuf = wgs->alt_numberpad_accumulator;
                            term_keyinputw(wgs->term, &keybuf, 1);
                        } else {
                            char ch = (char) wgs->alt_numberpad_accumulator;
                            /*
                             * We need not bother about stdin
                             * backlogs here, because in GUI PuTTY
                             * we can't do anything about it
                             * anyway; there's no means of asking
                             * Windows to hold off on KEYDOWN
                             * messages. We _have_ to buffer
                             * everything we're sent.
                             */
                            term_keyinput(wgs->term, -1, &ch, 1);
                        }
                        wgs->alt_numberpad_accumulator = 0;
                    } else {
                        term_keyinputw(wgs->term, &wch, 1);
                    }
                } else {
                    if (capsOn && wch < 0x80) {
                        WCHAR cbuf[2];
                        cbuf[0] = 27;
                        cbuf[1] = xlat_uskbd2cyrllic(wch);
                        term_keyinputw(
                            wgs->term, cbuf+!left_alt, 1+!!left_alt);
                    } else {
                        WCHAR cbuf[2];
                        cbuf[0] = '\033';
                        cbuf[1] = wch;
                        term_keyinputw(
                            wgs->term, cbuf +!left_alt, 1+!!left_alt);
                    }
                }
                win32_term_show_mouseptr(wgs, false);
            }

            return p - output;
        }
    }

    /*
     * ALT alone may or may not want to bring up the System menu.
     * If it's not meant to, we return 0 on presses or releases of
     * ALT, to show that we've swallowed the keystroke. Otherwise
     * we return -1, which means Windows will give the keystroke
     * its default handling (i.e. bring up the System menu).
     */
    if (wParam == VK_MENU && !conf_get_bool(wgs->conf, CONF_alt_only))
        return 0;

    return -1;
}
