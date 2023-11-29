/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>
#include <stdlib.h>  // abs()

#include "app/dtmf.h"
#ifdef ENABLE_AM_FIX_SHOW_DATA
	#include "am_fix.h"
#endif
#include "bitmaps.h"
#include "board.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "font.h"
#include "functions.h"
#include "helper/battery.h"
#ifdef ENABLE_MDC1200
	#include "mdc1200.h"
#endif
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/menu.h"
#include "ui/ui.h"

center_line_t g_center_line = CENTER_LINE_NONE;

// ***************************************************************************

void draw_bar(uint8_t *line, const int len, const int max_width)
{
	int i;
	#if 0
		// solid bar
		for (i = 0; i < max_width; i++)
			line[i] = (i > len) ? ((i & 1) == 0) ? 0x41 : 0x00 : ((i & 1) == 0) ? 0x7f : 0x3e;
	#elif 0
		// knuled bar
		for (i = 0; i < max_width; i += 2)
			line[i] = (i <= len) ? 0x7f : 0x41;
	#else
		// segmented bar
		for (i = 0; i < max_width; i += 4)
		{
			for (int k = i - 4; k < i && k < len; k++)
			{
				if (k >= 0)
//					line[k] = (k < (i - 1)) ? 0x7f : 0x00;
					if (k < (i - 1))
						line[k] = 0x3e;
			}
		}
		// top/bottom lines
		for (i = 0; i < len; i += 2)
			line[i] |= 0x41;
		for (i &= ~3u ; i < max_width; i += 4)
			line[i] = 0x41;
	#endif
}

#ifdef ENABLE_TX_TIMEOUT_BAR
	bool UI_DisplayTXCountdown(const bool now)
	{
		unsigned int timeout_secs = 0;

		if (g_current_function != FUNCTION_TRANSMIT || g_current_display_screen != DISPLAY_MAIN)
			return false;

		if (g_center_line != CENTER_LINE_NONE && g_center_line != CENTER_LINE_TX_TIMEOUT)
			return false;

		if (g_eeprom.tx_timeout_timer == 0)
			timeout_secs = 30;   // 30 sec
		else
		if (g_eeprom.tx_timeout_timer < (ARRAY_SIZE(g_sub_menu_tx_timeout) - 1))
			timeout_secs = 60 * g_eeprom.tx_timeout_timer;  // minutes
		else
			timeout_secs = 60 * 15;  // 15 minutes

		if (timeout_secs == 0 || g_tx_timer_tick_500ms == 0)
			return false;

		{
			const unsigned int line      = 3;
			const unsigned int txt_width = 7 * 6;                 // 6 text chars
			const unsigned int bar_x     = 2 + txt_width + 4;     // X coord of bar graph
			const unsigned int bar_width = LCD_WIDTH - 1 - bar_x;
			const unsigned int secs      = g_tx_timer_tick_500ms / 2;
			const unsigned int level     = ((secs * bar_width) + (timeout_secs / 2)) / timeout_secs;   // with rounding
//			const unsigned int level     = (((timeout_secs - secs) * bar_width) + (timeout_secs / 2)) / timeout_secs;   // with rounding
			const unsigned int len       = (level <= bar_width) ? level : bar_width;
			uint8_t           *p_line    = g_frame_buffer[line];
			char               s[17];

			if (now)
				memset(p_line, 0, LCD_WIDTH);

			sprintf(s, "TX %u", secs);
			#ifdef ENABLE_SMALL_BOLD
				UI_PrintStringSmallBold(s, 2, 0, line);
			#else
				UI_PrintStringSmall(s, 2, 0, line);
			#endif

			draw_bar(p_line + bar_x, len, bar_width);

			if (now)
				ST7565_BlitFullScreen();
		}

		return true;
	}
#endif

void UI_drawBars(uint8_t *p, const unsigned int level)
{
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wimplicit-fallthrough="

	switch (level)
	{
		default:
		case 7: memcpy(p + 20, BITMAP_ANTENNA_LEVEL6, sizeof(BITMAP_ANTENNA_LEVEL6));
		case 6: memcpy(p + 17, BITMAP_ANTENNA_LEVEL5, sizeof(BITMAP_ANTENNA_LEVEL5));
		case 5: memcpy(p + 14, BITMAP_ANTENNA_LEVEL4, sizeof(BITMAP_ANTENNA_LEVEL4));
		case 4: memcpy(p + 11, BITMAP_ANTENNA_LEVEL3, sizeof(BITMAP_ANTENNA_LEVEL3));
		case 3: memcpy(p +  8, BITMAP_ANTENNA_LEVEL2, sizeof(BITMAP_ANTENNA_LEVEL2));
		case 2: memcpy(p +  5, BITMAP_ANTENNA_LEVEL1, sizeof(BITMAP_ANTENNA_LEVEL1));
		case 1: memcpy(p +  0, BITMAP_ANTENNA,        sizeof(BITMAP_ANTENNA)); break;
		case 0: memset(p +  0, 0,                     sizeof(BITMAP_ANTENNA)); break;
	}

	#pragma GCC diagnostic pop
}

#ifdef ENABLE_TX_AUDIO_BAR

	// linear search, ascending, using addition
	uint16_t isqrt(const uint32_t y)
	{
		uint16_t L = 0;
		uint32_t a = 1;
		uint32_t d = 3;
		while (a <= y)
		{
			a += d;	// (a + 1) ^ 2
			d += 2;
			L += 1;
		}
		return L;
	}

	bool UI_DisplayAudioBar(const bool now)
	{
		if (g_current_function != FUNCTION_TRANSMIT || g_current_display_screen != DISPLAY_MAIN)
			return false;

		if (g_center_line != CENTER_LINE_NONE && g_center_line != CENTER_LINE_AUDIO_BAR)
			return false;

		if (g_dtmf_call_state != DTMF_CALL_STATE_NONE)
			return false;

		#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
			if (g_alarm_state != ALARM_STATE_OFF)
				return false;
		#endif

		if (g_setting_mic_bar)
		{
			const unsigned int line      = 3;
			const unsigned int txt_width = 7 * 3;                 // 3 text chars
			const unsigned int bar_x     = 2 + txt_width + 4;     // X coord of bar graph
			const unsigned int bar_width = LCD_WIDTH - 1 - bar_x;
			const unsigned int secs      = g_tx_timer_tick_500ms / 2;
			uint8_t           *p_line    = g_frame_buffer[line];
			char               s[16];

			if (now)
				memset(p_line, 0, LCD_WIDTH);

			// TX timeout seconds
			sprintf(s, "%3u", secs);
			#ifdef ENABLE_SMALL_BOLD
				UI_PrintStringSmallBold(s, 2, 0, line);
			#else
				UI_PrintStringSmall(s, 2, 0, line);
			#endif

			{	// TX audio level

				const unsigned int voice_amp  = BK4819_GetVoiceAmplitudeOut();  // 15:0

//				const unsigned int max        = 65535;
//				const unsigned int level      = ((voice_amp * bar_width) + (max / 2)) / max;            // with rounding
//				const unsigned int len        = (level <= bar_width) ? level : bar_width;

				// make non-linear to make more sensitive at low values
				const unsigned int level      = voice_amp * 8;
				const unsigned int sqrt_level = isqrt((level < 65535) ? level : 65535);
				const unsigned int len        = (sqrt_level <= bar_width) ? sqrt_level : bar_width;

				draw_bar(p_line + bar_x, len, bar_width);

				if (now)
					ST7565_BlitFullScreen();
			}
		}

		return true;
	}
#endif

#ifdef ENABLE_RX_SIGNAL_BAR
	bool UI_DisplayRSSIBar(const int16_t rssi, const bool now)
	{
		if (g_setting_rssi_bar)
		{
			const int16_t      s0_dBm       = -130;                  // S0 .. base level
			const unsigned int txt_width    = 7 * 8;                 // 8 text chars
			const unsigned int bar_x        = 2 + txt_width + 4;     // X coord of bar graph

			const unsigned int line         = 3;
			uint8_t           *p_line        = g_frame_buffer[line];
			char               str[16];

			const char plus[] = {
				0b00011000,
				0b00011000,
				0b01111110,
				0b01111110,
				0b01111110,
				0b00011000,
				0b00011000,
			};

			const char hollowBar[] = {
				0b01111111,
				0b01000001,
				0b01000001,
				0b01111111
			};

			#ifdef ENABLE_KEYLOCK
			if (g_eeprom.key_lock && g_keypad_locked > 0)
				return false;     // display is in use
			#endif

			if (g_current_function == FUNCTION_TRANSMIT ||
				g_current_display_screen != DISPLAY_MAIN ||
				g_dtmf_call_state != DTMF_CALL_STATE_NONE)
				return false;     // display is in use

			if (now)
				memset(p_line, 0, LCD_WIDTH);

			const int8_t dBmCorrTable[7] = {
				-15, // band 1
				-25, // band 2
				-20, // band 3
				-4,  // band 4
				-7,  // band 5
				-6,  // band 6
				-1   // band 7
			};

			const int16_t rssi_dBm = (rssi / 2) - 160 + dBmCorrTable[g_rx_vfo->band];

			const uint8_t s_level = MIN(MAX((rssi_dBm - s0_dBm) / 6, 0), 9); // S0 - S9
			uint8_t overS9dBm = MIN(MAX(rssi_dBm - (s0_dBm + 9*6), 0), 99);
			uint8_t overS9Bars = MIN(overS9dBm/10, 4);

			if(overS9Bars == 0) {
				sprintf(str, "% 4d S%d", rssi_dBm, s_level);
				UI_PrintStringSmall(str, 2, 0, line);
			}
			else {
				sprintf(str, "% 4d  %2d", rssi_dBm, overS9dBm);
				UI_PrintStringSmall(str, 2, 0, line);
				memcpy(p_line + 2 + 7*5, &plus, ARRAY_SIZE(plus));
			}

			uint8_t level = MIN(s_level + overS9Bars, 13);

			for(uint8_t i = 0; i < level; i++) {
				if(i < 9) {
					for(uint8_t j = 0; j < 4; j++)
						p_line[bar_x + i * 5 + j] = (~(0x7F >> (i+1))) & 0x7F;
				} else {
					memcpy(p_line + (bar_x + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
				}
			}

			if (now)
				ST7565_BlitFullScreen();

			return true;
		}

		return false;
	}
#endif

void UI_update_rssi(const int16_t rssi, const int vfo)
{
#ifdef ENABLE_RX_SIGNAL_BAR
	if (g_center_line == CENTER_LINE_RSSI)
	{	// optional larger RSSI dBm, S-point and bar level
		if (g_current_function == FUNCTION_RECEIVE || g_current_function == FUNCTION_MONITOR)
		{
			UI_DisplayRSSIBar(rssi, true);
		}
	}
#endif

	{	// original little RS bars

//		const int16_t dBm        = (rssi / 2) - 160;
		const uint8_t Line       = (vfo == 0) ? 3 : 7;
		uint8_t      *p_line     = g_frame_buffer[Line - 1];
		uint8_t       rssi_level = 0;

		// TODO: sort out all 8 values from the eeprom

		#if 1
			const unsigned int band = g_rx_vfo->band;
			const int16_t level0  = g_eeprom_rssi_calib[band][0];
			const int16_t level1  = g_eeprom_rssi_calib[band][1];
			const int16_t level2  = g_eeprom_rssi_calib[band][2];
			const int16_t level3  = g_eeprom_rssi_calib[band][3];
		#else
			const int16_t level0  = (-115 + 160) * 2;   // -115dBm
			const int16_t level1  = ( -89 + 160) * 2;   //  -89dBm
			const int16_t level2  = ( -64 + 160) * 2;   //  -64dBm
			const int16_t level3  = ( -39 + 160) * 2;   //  -39dBm
		#endif
		// create intermediate threshold values (linear interpolation) to make full use of the available RSSI bars/graphics
		const int16_t level01 = (level0 + level1) / 2;
		const int16_t level12 = (level1 + level2) / 2;
		const int16_t level23 = (level2 + level3) / 2;

		g_vfo_rssi[vfo] = rssi;

		if (rssi >= level3)
			rssi_level = 7;
		else
		if (rssi >= level23)
			rssi_level = 6;
		else
		if (rssi >= level2)
			rssi_level = 5;
		else
		if (rssi >= level12)
			rssi_level = 4;
		else
		if (rssi >= level1)
			rssi_level = 3;
		else
		if (rssi >= level01)
			rssi_level = 2;
		else
		if (rssi >= level0 ||
		    g_current_function == FUNCTION_MONITOR ||
		    g_current_function == FUNCTION_NEW_RECEIVE)
		{
			rssi_level = 1;
		}

		if (g_vfo_rssi_bar_level[vfo] == rssi_level)
			return;

		g_vfo_rssi_bar_level[vfo] = rssi_level;

		// **********************************************************

		#ifdef ENABLE_KEYLOCK
		if (g_eeprom.key_lock && g_keypad_locked > 0)
			return;    // display is in use
		#endif

		if (g_current_function == FUNCTION_TRANSMIT || g_current_display_screen != DISPLAY_MAIN)
			return;    // display is in use

		p_line = g_frame_buffer[Line - 1];

		memset(p_line, 0, 23);

		// untested !!!

		if (rssi_level == 0)
			p_line = NULL;
		else
			UI_drawBars(p_line, rssi_level);

		ST7565_DrawLine(0, Line, 23, p_line);
	}
}

// ***************************************************************************

void big_freq(const uint32_t frequency, const unsigned int x, const unsigned int line)
{
	char str[9];

	NUMBER_ToDigits(frequency, str);

	// show the main large frequency digits
	UI_DisplayFrequency(str, x, line, false, false);

	// show the remaining 2 small frequency digits
	#ifdef ENABLE_TRIM_TRAILING_ZEROS
	{
		unsigned int small_num = 2;
		if (str[7] == 0)
		{
			small_num--;
			if (str[6] == 0)
				small_num--;
		}
		UI_Displaysmall_digits(small_num, str + 6, x + 81, line + 1, true);
	}
	#else
		UI_Displaysmall_digits(2, str + 6, x + 81, line + 1, true);
	#endif
}

void UI_DisplayMain(void)
{
	#ifndef ENABLE_BIG_FREQ
		const unsigned int smallest_char_spacing = ARRAY_SIZE(g_font3x5[0]) + 1;
	#endif
	const unsigned int line0 = 0;  // text screen line
	const unsigned int line1 = 4;
	char               str[22];
	unsigned int       vfo_num;

	g_center_line = CENTER_LINE_NONE;

//	#ifdef SINGLE_VFO_CHAN
//		const bool single_vfo = (g_eeprom.dual_watch == DUAL_WATCH_OFF && g_eeprom.cross_vfo_rx_tx == CROSS_BAND_OFF) ? true : false;
//	#else
		const bool single_vfo = false;
//	#endif

	// clear the screen
	memset(g_frame_buffer, 0, sizeof(g_frame_buffer));

	if (g_serial_config_tick_500ms > 0)
	{
		backlight_turn_on(10);		// 5 seconds
		UI_PrintString("UART", 0, LCD_WIDTH, 1, 8);
		UI_PrintString("CONFIG COMMS", 0, LCD_WIDTH, 3, 8);
		ST7565_BlitFullScreen();
		return;
	}

	#ifdef ENABLE_KEYLOCK
	if (g_eeprom.key_lock && g_keypad_locked > 0)
	{	// tell user how to unlock the keyboard
		backlight_turn_on(10);     // 5 seconds
		UI_PrintString("Long press #", 0, LCD_WIDTH, 1, 8);
		UI_PrintString("to unlock",    0, LCD_WIDTH, 3, 8);
		ST7565_BlitFullScreen();
		return;
	}
	#endif

	for (vfo_num = 0; vfo_num < 2; vfo_num++)
	{
		const unsigned int line       = (vfo_num == 0) ? line0 : line1;
		unsigned int       channel    = g_eeprom.tx_vfo;
//		unsigned int       tx_channel = (g_eeprom.cross_vfo_rx_tx == CROSS_BAND_OFF) ? g_eeprom.rx_vfo : g_eeprom.tx_vfo;
		const bool         same_vfo   = (channel == vfo_num) ? true : false;
		uint8_t           *p_line0    = g_frame_buffer[line + 0];
		uint8_t           *p_line1    = g_frame_buffer[line + 1];
		unsigned int       mode       = 0;
		unsigned int       state;

		if (single_vfo)
		{	// we're in single VFO mode - screen is dedicated to just one VFO

			if (!same_vfo)
				continue;  // skip the unused vfo


		}

		if (g_eeprom.dual_watch != DUAL_WATCH_OFF && g_rx_vfo_is_active)
			channel = g_eeprom.rx_vfo;    // we're currently monitoring the other VFO

		if (channel != vfo_num)
		{
			if (g_dtmf_call_state != DTMF_CALL_STATE_NONE || g_dtmf_is_tx || g_dtmf_input_mode)
			{	// show DTMF stuff

				char contact[17];

				if (!g_dtmf_input_mode)
				{
					memset(contact, 0, sizeof(contact));
					if (g_dtmf_call_state == DTMF_CALL_STATE_CALL_OUT)
					{
						strcpy(str, (g_dtmf_state == DTMF_STATE_CALL_OUT_RSP) ? "CALL OUT RESP" : "CALL OUT");
					}
					else
					if (g_dtmf_call_state == DTMF_CALL_STATE_RECEIVED || g_dtmf_call_state == DTMF_CALL_STATE_RECEIVED_STAY)
					{
						const bool found = DTMF_FindContact(g_dtmf_caller, contact);
						contact[8] = 0;
						sprintf(str, "FROM %s", found ? contact : g_dtmf_caller);
					}
					else
					if (g_dtmf_is_tx)
					{
						strcpy(str, (g_dtmf_state == DTMF_STATE_TX_SUCC) ? "DTMF TX SUCC" : "DTMF TX");
					}
				}
				else
				{
					sprintf(str, ">%s", g_dtmf_input_box);
				}
				str[16] = 0;
				UI_PrintString(str, 2, 0, 0 + (vfo_num * 3), 8);

				memset(str,  0, sizeof(str));
				if (!g_dtmf_input_mode)
				{
					memset(contact, 0, sizeof(contact));
					if (g_dtmf_call_state == DTMF_CALL_STATE_CALL_OUT)
					{
						const bool found = DTMF_FindContact(g_dtmf_string, contact);
						contact[15] = 0;
						sprintf(str, ">%s", found ? contact : g_dtmf_string);
					}
					else
					if (g_dtmf_call_state == DTMF_CALL_STATE_RECEIVED || g_dtmf_call_state == DTMF_CALL_STATE_RECEIVED_STAY)
					{
						const bool found = DTMF_FindContact(g_dtmf_callee, contact);
						contact[15] = 0;
						sprintf(str, ">%s", found ? contact : g_dtmf_callee);
					}
					else
					if (g_dtmf_is_tx)
					{
						sprintf(str, ">%s", g_dtmf_string);
					}
				}
				str[16] = 0;
				UI_PrintString(str, 2, 0, 2 + (vfo_num * 3), 8);

				g_center_line = CENTER_LINE_IN_USE;
				continue;
			}

			// highlight the selected/used VFO with a marker
			if (!single_vfo && same_vfo)
				memcpy(p_line0 + 0, BITMAP_VFO_DEFAULT, sizeof(BITMAP_VFO_DEFAULT));
			else
			if (g_eeprom.cross_vfo_rx_tx != CROSS_BAND_OFF)
				memcpy(p_line0 + 0, BITMAP_VFO_NOT_DEFAULT, sizeof(BITMAP_VFO_NOT_DEFAULT));
		}
		else
		if (!single_vfo)
		{	// highlight the selected/used VFO with a marker
			if (same_vfo)
				memcpy(p_line0 + 0, BITMAP_VFO_DEFAULT, sizeof(BITMAP_VFO_DEFAULT));
			else
			//if (g_eeprom.cross_vfo_rx_tx != CROSS_BAND_OFF)
				memcpy(p_line0 + 0, BITMAP_VFO_NOT_DEFAULT, sizeof(BITMAP_VFO_NOT_DEFAULT));
		}

		if (g_current_function == FUNCTION_TRANSMIT)
		{	// transmitting

			#ifdef ENABLE_ALARM
				if (g_alarm_state == ALARM_STATE_ALARM)
					mode = 1;
				else
			#endif
			{
				channel = (g_eeprom.cross_vfo_rx_tx == CROSS_BAND_OFF) ? g_eeprom.rx_vfo : g_eeprom.tx_vfo;
				if (channel == vfo_num)
				{	// show the TX symbol
					mode = 1;
					#ifdef ENABLE_SMALL_BOLD
						UI_PrintStringSmallBold("TX", 14, 0, line);
					#else
						UI_PrintStringSmall("TX", 14, 0, line);
					#endif
				}
			}
		}
		else
		{	// receiving .. show the RX symbol
			mode = 2;
			if ((g_current_function == FUNCTION_RECEIVE ||
			     g_current_function == FUNCTION_MONITOR ||
			     g_current_function == FUNCTION_NEW_RECEIVE) &&
			     g_eeprom.rx_vfo == vfo_num)
			{
				#ifdef ENABLE_SMALL_BOLD
					UI_PrintStringSmallBold("RX", 14, 0, line);
				#else
					UI_PrintStringSmall("RX", 14, 0, line);
				#endif
			}
		}

		if (g_eeprom.screen_channel[vfo_num] <= USER_CHANNEL_LAST)
		{	// channel mode
			const unsigned int x = 2;
			const bool inputting = (g_input_box_index == 0 || g_eeprom.tx_vfo != vfo_num) ? false : true;
			if (!inputting)
				NUMBER_ToDigits(g_eeprom.screen_channel[vfo_num] + 1, str);  // show the memory channel number
			else
				memcpy(str + 5, g_input_box, 3);                            // show the input text
			UI_PrintStringSmall("M", x, 0, line + 1);
			UI_Displaysmall_digits(3, str + 5, x + 7, line + 1, inputting);
		}
		else
		if (IS_FREQ_CHANNEL(g_eeprom.screen_channel[vfo_num]))
		{	// frequency mode
			// show the frequency band number
			const unsigned int x = 2;	// was 14
//			sprintf(String, "FB%u", 1 + g_eeprom.screen_channel[vfo_num] - FREQ_CHANNEL_FIRST);
			sprintf(str, "VFO%u", 1 + g_eeprom.screen_channel[vfo_num] - FREQ_CHANNEL_FIRST);
			UI_PrintStringSmall(str, x, 0, line + 1);
		}
		#ifdef ENABLE_NOAA
			else
			{
				if (g_input_box_index == 0 || g_eeprom.tx_vfo != vfo_num)
				{	// channel number
					sprintf(str, "N%u", 1 + g_eeprom.screen_channel[vfo_num] - NOAA_CHANNEL_FIRST);
				}
				else
				{	// user entering channel number
					sprintf(str, "N%u%u", '0' + g_input_box[0], '0' + g_input_box[1]);
				}
				UI_PrintStringSmall(str, 7, 0, line + 1);
			}
		#endif

		// ************

		state = g_vfo_state[vfo_num];

		#ifdef ENABLE_ALARM
			if (g_current_function == FUNCTION_TRANSMIT && g_alarm_state == ALARM_STATE_ALARM)
			{
				channel = (g_eeprom.cross_vfo_rx_tx == CROSS_BAND_OFF) ? g_eeprom.rx_vfo : g_eeprom.tx_vfo;
				if (channel == vfo_num)
					state = VFO_STATE_ALARM;
			}
		#endif

		if (state != VFO_STATE_NORMAL)
		{
			const char *state_list[] = {"", "BUSY", "BAT LOW", "TX DISABLE", "TIMEOUT", "ALARM", "VOLT HIGH"};
			if (state < ARRAY_SIZE(state_list))
				UI_PrintString(state_list[state], 31, 0, line, 8);
		}
		else
		if (g_input_box_index > 0 && IS_FREQ_CHANNEL(g_eeprom.screen_channel[vfo_num]) && g_eeprom.tx_vfo == vfo_num)
		{	// user entering a frequency
			UI_DisplayFrequency(g_input_box, 32, line, true, false);

//			g_center_line = CENTER_LINE_IN_USE;
		}
		else
		{
			const unsigned int x = 32;

			uint32_t frequency = g_eeprom.vfo_info[vfo_num].p_rx->frequency;
			if (g_current_function == FUNCTION_TRANSMIT)
			{	// transmitting
				channel = (g_eeprom.cross_vfo_rx_tx == CROSS_BAND_OFF) ? g_eeprom.rx_vfo : g_eeprom.tx_vfo;
				if (channel == vfo_num)
					frequency = g_eeprom.vfo_info[vfo_num].p_tx->frequency;
			}

			if (g_eeprom.screen_channel[vfo_num] <= USER_CHANNEL_LAST)
			{	// it's a channel

				switch (g_eeprom.channel_display_mode)
				{
					case MDF_FREQUENCY:	// just channel frequency

						#ifdef ENABLE_BIG_FREQ
							big_freq(frequency, x, line);
						#else
							// show the frequency in the main font
							sprintf(str, "%03u.%05u", frequency / 100000, frequency % 100000);
							#ifdef ENABLE_TRIM_TRAILING_ZEROS
								NUMBER_trim_trailing_zeros(str);
							#endif
							UI_PrintString(str, x, 0, line, 8);
						#endif

						break;

					case MDF_CHANNEL:	// just channel number

						sprintf(str, "CH-%03u", g_eeprom.screen_channel[vfo_num] + 1);
						UI_PrintString(str, x, 0, line, 8);

						break;

					case MDF_NAME:		// channel name
					case MDF_NAME_FREQ:	// channel name and frequency

						BOARD_fetchChannelName(str, g_eeprom.screen_channel[vfo_num]);
						if (str[0] == 0)
						{	// no channel name available, channel number instead
							sprintf(str, "CH-%03u", 1 + g_eeprom.screen_channel[vfo_num]);
						}

						if (g_eeprom.channel_display_mode == MDF_NAME)
						{	// just the name
							UI_PrintString(str, x + 4, 0, line, 8);
						}
						else
						{	// name & frequency

							// name
							#ifdef ENABLE_SMALL_BOLD
								UI_PrintStringSmallBold(str, x + 4, 0, line);
							#else
								UI_PrintStringSmall(str, x + 4, 0, line);
							#endif

							// frequency
							sprintf(str, "%03u.%05u", frequency / 100000, frequency % 100000);
							#ifdef ENABLE_TRIM_TRAILING_ZEROS
								NUMBER_trim_trailing_zeros(str);
							#endif
							UI_PrintStringSmall(str, x + 4, 0, line + 1);
						}

						break;
				}
			}
			else
//			if (IS_FREQ_CHANNEL(g_eeprom.screen_channel[vfo_num]))
			{	// frequency mode
				#ifdef ENABLE_BIG_FREQ
					big_freq(frequency, x, line);
				#else
					// show the frequency in the main font

					sprintf(str, "%03u.%05u", frequency / 100000, frequency % 100000);
					#ifdef ENABLE_TRIM_TRAILING_ZEROS
						NUMBER_trim_trailing_zeros(str);
					#endif
					UI_PrintString(str, x, 0, line, 8);

				#endif
			}

			// show channel symbols

			if (g_eeprom.screen_channel[vfo_num] <= USER_CHANNEL_LAST)
			//if (IS_NOT_NOAA_CHANNEL(g_eeprom.screen_channel[vfo_num]))
			{	// it's a user channel or VFO

				unsigned int x = LCD_WIDTH - 1 - sizeof(BITMAP_SCANLIST2) - sizeof(BITMAP_SCANLIST1);

				const uint8_t attributes = g_user_channel_attributes[g_eeprom.screen_channel[vfo_num]];

				if (attributes & USER_CH_SCANLIST1)
					memcpy(p_line0 + x, BITMAP_SCANLIST1, sizeof(BITMAP_SCANLIST1));
				x += sizeof(BITMAP_SCANLIST1);

				if (attributes & USER_CH_SCANLIST2)
					memcpy(p_line0 + x, BITMAP_SCANLIST2, sizeof(BITMAP_SCANLIST2));
				//x += sizeof(BITMAP_SCANLIST2);
			}

			#ifdef ENABLE_BIG_FREQ

				// no room for these symbols

			#else
			{
				unsigned int x = LCD_WIDTH + LCD_WIDTH - 1 - (smallest_char_spacing * 1) - (smallest_char_spacing * 4);

				if (IS_FREQ_CHANNEL(g_eeprom.screen_channel[vfo_num]))
				{
					//g_eeprom.vfo_info[vfo_num].freq_in_channel = BOARD_find_channel(frequency);
					if (g_eeprom.vfo_info[vfo_num].freq_in_channel <= USER_CHANNEL_LAST)
					{	// the channel number that contains this VFO frequency
						sprintf(str, "%03u", 1 + g_eeprom.vfo_info[vfo_num].freq_in_channel);
						UI_PrintStringSmallest(str, x, (line + 0) * 8, false, true);
					}
				}
				x += smallest_char_spacing * 4;

				if (g_eeprom.vfo_info[vfo_num].compand)
					UI_PrintStringSmallest("C", x, (line + 0) * 8, false, true);
				//x += smallest_char_spacing * 1;
			}
			#endif
		}

		// ************

		{	// show the TX/RX level
			uint8_t Level = 0;

			if (mode == 1)
			{	// TX power level
				switch (g_rx_vfo->output_power)
				{
					case OUTPUT_POWER_LOW:  Level = 2; break;
					case OUTPUT_POWER_MID:  Level = 4; break;
					case OUTPUT_POWER_HIGH: Level = 6; break;
				}
			}
			else
			if (mode == 2)
			{	// RX signal level
				//#ifndef ENABLE_RX_SIGNAL_BAR
					// antenna bar graph
					if (g_vfo_rssi_bar_level[vfo_num] > 0)
						Level = g_vfo_rssi_bar_level[vfo_num];
				//#endif
			}

			UI_drawBars(p_line1 + LCD_WIDTH, Level);
		}

		// ************

		str[0] = '\0';
		if (g_eeprom.vfo_info[vfo_num].am_mode)
		{	// show the AM symbol
			strcpy(str, "AM");
		}
		else
		{	// or show the CTCSS/DCS symbol
			const freq_config_t *pConfig = (mode == 1) ? g_eeprom.vfo_info[vfo_num].p_tx : g_eeprom.vfo_info[vfo_num].p_rx;
			const unsigned int code_type = pConfig->code_type;
			const char *code_list[] = {"", "CT", "DCS", "DCR"};
			if (code_type < ARRAY_SIZE(code_list))
				strcpy(str, code_list[code_type]);
		}
		UI_PrintStringSmall(str, LCD_WIDTH + 24, 0, line + 1);

		#ifdef ENABLE_TX_WHEN_AM
			if (state == VFO_STATE_NORMAL || state == VFO_STATE_ALARM)
		#else
			if ((state == VFO_STATE_NORMAL || state == VFO_STATE_ALARM) && !g_eeprom.vfo_info[vfo_num].am_mode) // not allowed to TX if in AM mode
		#endif
		{
			if (FREQUENCY_tx_freq_check(g_eeprom.vfo_info[vfo_num].p_tx->frequency) == 0)
			{
				// show the TX power
				const char pwr_list[] = "LMH";
				const unsigned int i = g_eeprom.vfo_info[vfo_num].output_power;
				str[0] = (i < ARRAY_SIZE(pwr_list)) ? pwr_list[i] : '\0';
				str[1] = '\0';
				UI_PrintStringSmall(str, LCD_WIDTH + 46, 0, line + 1);

				if (g_eeprom.vfo_info[vfo_num].freq_config_rx.frequency != g_eeprom.vfo_info[vfo_num].freq_config_tx.frequency)
				{	// show the TX offset symbol
					const char dir_list[] = "\0+-";
					const unsigned int i = g_eeprom.vfo_info[vfo_num].tx_offset_freq_dir;
					str[0] = (i < sizeof(dir_list)) ? dir_list[i] : '?';
					str[1] = '\0';
					UI_PrintStringSmall(str, LCD_WIDTH + 54, 0, line + 1);
				}
			}
		}

		// show the TX/RX reverse symbol
		if (g_eeprom.vfo_info[vfo_num].frequency_reverse)
			UI_PrintStringSmall("R", LCD_WIDTH + 62, 0, line + 1);

		{	// show the narrow band symbol
			str[0] = '\0';
			if (g_eeprom.vfo_info[vfo_num].channel_bandwidth == BANDWIDTH_NARROW)
			{
				str[0] = 'N';
				str[1] = '\0';
			}
			UI_PrintStringSmall(str, LCD_WIDTH + 70, 0, line + 1);
		}

		// show the DTMF decoding symbol
		#ifdef ENABLE_KILL_REVIVE
			if (g_eeprom.vfo_info[vfo_num].dtmf_decoding_enable || g_setting_radio_disabled)
				UI_PrintStringSmall("DTMF", LCD_WIDTH + 78, 0, line + 1);
		#else
			if (g_eeprom.vfo_info[vfo_num].dtmf_decoding_enable)
				UI_PrintStringSmall("DTMF", LCD_WIDTH + 78, 0, line + 1);
				//UI_PrintStringSmall4x5("DTMF", LCD_WIDTH + 78, 0, line + 1);   // font table is currently wrong
				//UI_PrintStringSmallest("DTMF", LCD_WIDTH + 78, (line + 1) * 8, false, true);
		#endif

		// show the audio scramble symbol
		if (g_eeprom.vfo_info[vfo_num].scrambling_type > 0 && g_setting_scramble_enable)
			UI_PrintStringSmall("SCR", LCD_WIDTH + 106, 0, line + 1);
	}

	if (g_center_line == CENTER_LINE_NONE &&
		g_current_display_screen == DISPLAY_MAIN &&
		g_dtmf_call_state == DTMF_CALL_STATE_NONE)
	{	// we're free to use the middle line

		const bool rx = (g_current_function == FUNCTION_RECEIVE ||
		                 g_current_function == FUNCTION_MONITOR ||
		                 g_current_function == FUNCTION_NEW_RECEIVE);

		#ifdef ENABLE_TX_TIMEOUT_BAR
			// show the TX timeout count down
			if (UI_DisplayTXCountdown(false))
			{
				g_center_line = CENTER_LINE_TX_TIMEOUT;
			}
			else
		#endif

		#ifdef ENABLE_TX_AUDIO_BAR
			// show the TX audio level
			if (UI_DisplayAudioBar(false))
			{
				g_center_line = CENTER_LINE_AUDIO_BAR;
			}
			else
		#endif

		#ifdef ENABLE_MDC1200
			if (mdc1200_rx_ready_tick_500ms > 0)
			{
				g_center_line = CENTER_LINE_MDC1200;
				sprintf(str, "MDC1200 %02X %02X %04X", mdc1200_op, mdc1200_arg, mdc1200_unit_id);
				#ifdef ENABLE_SMALL_BOLD
					UI_PrintStringSmallBold(str, 2, 0, 3);
				#else
					UI_PrintStringSmall(str, 2, 0, 3);
				#endif
			}
			else
		#endif

		#if defined(ENABLE_AM_FIX) && defined(ENABLE_AM_FIX_SHOW_DATA)
			// show the AM-FIX debug data
			if (rx && g_eeprom.vfo_info[g_eeprom.rx_vfo].am_mode && g_setting_am_fix)
			{
				g_center_line = CENTER_LINE_AM_FIX_DATA;
				AM_fix_print_data(g_eeprom.rx_vfo, str);
				UI_PrintStringSmall(str, 2, 0, 3);
			}
			else
		#endif

		#ifdef ENABLE_RX_SIGNAL_BAR
			// show the RX RSSI dBm, S-point and signal strength bar graph
			if (rx && g_setting_rssi_bar)
			{
				g_center_line = CENTER_LINE_RSSI;
				UI_DisplayRSSIBar(g_current_rssi[g_eeprom.rx_vfo], false);
			}
			else
		#endif

		if (rx || g_current_function == FUNCTION_FOREGROUND || g_current_function == FUNCTION_POWER_SAVE)
		{
			#if 1
				if (g_setting_live_dtmf_decoder && g_dtmf_rx_live[0] != 0)
				{	// show live DTMF decode
					const unsigned int len = strlen(g_dtmf_rx_live);
					const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

					if (g_current_display_screen != DISPLAY_MAIN || g_dtmf_call_state != DTMF_CALL_STATE_NONE)
						return;

					g_center_line = CENTER_LINE_DTMF_DEC;

					strcpy(str, "DTMF ");
					strcat(str, g_dtmf_rx_live + idx);
					UI_PrintStringSmall(str, 2, 0, 3);
				}
			#else
				if (g_setting_live_dtmf_decoder && g_dtmf_rx_index > 0)
				{	// show live DTMF decode
					const unsigned int len = g_dtmf_rx_index;
					const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

					if (g_current_display_screen != DISPLAY_MAIN || g_dtmf_call_state != DTMF_CALL_STATE_NONE)
						return;

					g_center_line = CENTER_LINE_DTMF_DEC;

					strcpy(str, "DTMF ");
					strcat(str, g_dtmf_rx + idx);
					UI_PrintStringSmall(str, 2, 0, 3);
				}
			#endif

			#ifdef ENABLE_SHOW_CHARGE_LEVEL
				else
				if (g_charging_with_type_c)
				{	// show the battery charge state
					if (g_current_display_screen != DISPLAY_MAIN || g_dtmf_call_state != DTMF_CALL_STATE_NONE)
						return;

					g_center_line = CENTER_LINE_CHARGE_DATA;

					sprintf(str, "Charge %u.%02uV %u%%",
						g_battery_voltage_average / 100, g_battery_voltage_average % 100,
						BATTERY_VoltsToPercent(g_battery_voltage_average));
					UI_PrintStringSmall(str, 2, 0, 3);
				}
			#endif
		}
	}

	ST7565_BlitFullScreen();
}

// ***************************************************************************
