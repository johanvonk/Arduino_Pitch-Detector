/**
 * @brief Show MIDI data on piano roll
 * @file  pianoroll.cpp
 * Platform: Arduino UNO R3 using Arduino IDE
 * Documentation: http://www.coertvonk.com/technology/embedded/arduino-pitch-detector-13252
 *
 * GNU GENERAL PUBLIC LICENSE Version 3, check the file LICENSE for more information
 * (c) Copyright 2015-2016, Johan Vonk
 * All rights reserved.  Use of copyright notice does not imply publication.
 * All text above must be included in any redistribution
 **/

#include <Arduino.h>
#include <stdint.h>
#include <Adafruit_GFX.h>    // core graphics
#include <Adafruit_ST7735.h> // hardware-specific graphics
#include "config.h"
#include "debug.h"
#include "pitch.h"
#include "sample_t.h"
#include "segmentbuf.h"
#include "pianoroll.h"
#include "coordinate_t.h"

#if DST == DST_PIANOROLL

namespace {
											//   rrrr rggg gggb bbbb
	color_t const COLOR_NOTESTART = 0xF800; // 0b1111 1000 0000 0000  red
	color_t const COLOR_NOTE = 0x0700;      // 0b0000 0111 0000 0000  dark green
	color_t const COLOR_CURSOR = 0x001F;    // 0b0000 0000 0001 1111  blue
	color_t const COLOR_ROLLC = 0x2104;     // 0b0010 0001 0000 0100  dark gray
	color_t const COLOR_ROLLG = 0xC618;     // 0b1100 0110 0001 1000  gray
	color_t const COLOR_ROLLOTHER = 0xF79E; // 0b1111 0111 1001 1110  light gray
	color_t const COLOR_BG = 0xFFFF;        // 0b1111 1111 1111 1111  white

	Adafruit_ST7735 * _tft;

	struct _display_t {
		xCoordinate_t width;
		yCoordinate_t height;
	} _display;

	struct _distance_t {
		yCoordinate_t pitch2pitch;
		yCoordinate_t bottom2loPitch;
	} _distance;

	xCoordinate_t const CHAR_WIDTH = 6;
	yCoordinate_t const CHAR_HEIGHT = 8;

	xCoordinate_t const X_FIRSTNOTE = 2 * CHAR_WIDTH;

	segmentPitch_t const PITCH_MIN = Pitch::freq2pitch( Config::FREQ_MIN );
	segmentPitch_t const PITCH_MAX = Pitch::freq2pitch( Config::FREQ_MAX );

	segmentRelTime_t _msecPerPixel;
	segmentRelTime_t _msecOnScreen;
	absoluteTime_t _msecStart;

	void
	_resize( int width, int height )
	{
		_display.height = height;
		_display.width = width;

		segmentPitch_t const nrOfPos = PITCH_MAX - PITCH_MIN + 1;

		_distance.pitch2pitch = height / nrOfPos;
		_distance.bottom2loPitch = (height - nrOfPos*_distance.pitch2pitch) / 2;

		xCoordinate_t const sWidth = width - X_FIRSTNOTE;  // screen width [pixels]
		_msecOnScreen = 2912;                 // screen width [msec]
		_msecPerPixel = _msecOnScreen / sWidth;
	}


	INLINE xCoordinate_t const           // returns x-coordinate on display [0 .. screen width - 1]
	_time2x( absoluteTime_t const  t,    // note time
			 absoluteTime_t const  t0 )  // time on left of screen
	{
		xCoordinate_t const distance = t - _msecStart > t0 ? (t - _msecStart - t0) / _msecPerPixel : 0;

		return X_FIRSTNOTE + distance;
	}


	INLINE yCoordinate_t const              // returns y-coordinate on display
	_pitch2y( segmentPitch_t const pitch )  // midi pitch
	{
		yCoordinate_t const diff = (pitch - PITCH_MIN) * _distance.pitch2pitch;

		return _display.height - _distance.bottom2loPitch - diff;
	}


	void
	_displayRoll( xCoordinate_t const xLeft,
				  xCoordinate_t const xWidth )
	{
		xCoordinate_t xRight = xLeft + xWidth;

		for ( segmentPitch_t ii = PITCH_MIN; ii <= PITCH_MAX; ii++ ) {

			noteNr_t nr = static_cast<noteNr_t>(ii % 12);

			bool isC = (nr == noteNr_t::C);
			bool isG = (nr == noteNr_t::G);

			color_t const color = isC ? COLOR_ROLLC : isG ? COLOR_ROLLG : COLOR_ROLLOTHER;

			xCoordinate_t x = xLeft;
			yCoordinate_t const y = _pitch2y( ii );

			if ( x == 0 ) {
				if ( isC || isG ) {  // write a few note names on far left

					octaveNr_t octave = ii / 12;
					yCoordinate_t const cY = y - CHAR_HEIGHT / 2 + 1;

					_tft->drawChar( 0, cY, isC ? 'C' : 'G', color, color, 1 );
					_tft->drawChar( CHAR_WIDTH, cY, '0' + octave, color, color, 1 );
				}
				x = X_FIRSTNOTE;  // start lines right of the note names
			}
			_tft->drawFastHLine( x, y, xRight - xLeft, color );
		}
	}

}  // name space


void
PianoRoll::show( absoluteTime_t const  lastOffset,   // needed to calculate absolute times
				 SegmentBuf * const    segmentBuf )  // segment buffer containing notes
{
	absoluteTime_t const now = millis();

	absoluteTime_t const n = (now - _msecStart) / _msecOnScreen;  // #times cursor wrapped around
	absoluteTime_t const t0 = n * _msecOnScreen;                  // time corresponding to the most left position on screen
	xCoordinate_t const cursor = _time2x( now, t0 );
	uint_least8_t const startLen = 2;                           // first two pixels hi-light the note start

	// clear 1/20 of the screen width right of cursor
	xCoordinate_t const wipe = min( _display.width / 20, _display.width - cursor );
	_tft->fillRect( cursor, 0, wipe, _display.height, COLOR_BG );

	// draw line, just ahead of cursor
	_tft->drawFastVLine( cursor + 1, 0, _display.height, COLOR_CURSOR );

	// Redraw a msec positions left of cursor.  This is needed because a new
	// note is only recognized after it meets this minimum duration.  Until then, the note is shown
	// as part of the previous note (or rest).
	absoluteTime_t const maxLoopTime = 60;  // worst case is ~60msec per chunk [msec], increase if you see empty columns in the piano roll
	absoluteTime_t const drawInMsec = min( Config::MIN_SEGMENT_DURATION + maxLoopTime, (cursor - X_FIRSTNOTE)*_msecPerPixel );
	xCoordinate_t const drawInPixels = (xCoordinate_t)drawInMsec / _msecPerPixel;                             // 38 msec
	_tft->fillRect( cursor - drawInPixels, 0, drawInPixels, _display.height, COLOR_BG );  // erase, in case the pitch changed
	_displayRoll( cursor - drawInPixels, drawInPixels );

	uint_least8_t ii = 0;
	segment_t const * note;

	absoluteTime_t offset = lastOffset;

	while ( (note = segmentBuf->headPtr( ii++ )) &&  // there are more notes to show &&
			(offset > now - drawInMsec) ) {          // the note should be on screen

		absoluteTime_t const onset = offset - note->duration;

		xCoordinate_t const xLeft = _time2x( onset, t0 );
		xCoordinate_t const xWidth = _time2x( offset, t0 ) - xLeft;
		yCoordinate_t const yTop = _pitch2y( note->pitch ) + _distance.pitch2pitch / 2;
		yCoordinate_t const yHeight = _distance.pitch2pitch;

		_tft->fillRect( xLeft + startLen, yTop, xWidth - startLen, yHeight, COLOR_NOTE );
		_tft->fillRect( xLeft, yTop, startLen, yHeight, COLOR_NOTESTART );

		offset = onset - note->onset;  // note->onset is a relative time
	}
}

void
PianoRoll::clear( void )
{
	_tft->fillScreen( COLOR_BG );
	_displayRoll( 0, _display.width );
	_msecStart = millis();
}


void
PianoRoll::begin( uint_least8_t tftCS,  // SPI TFT Chip Select
				  uint_least8_t dc,     // SPI Data/Command
				  uint_least8_t reset ) // SPI Reset
{
	pinMode( tftCS, OUTPUT );
	_tft = new Adafruit_ST7735( tftCS, dc, reset );  // instantiate TFT driver
	_tft->initR( INITR_BLACKTAB );                   // initialize TFT (ST7735S chip, black tab)
	_tft->setRotation( 3 );                          // make (0,0) corresponds to top-right
	_resize( _tft->width(), _tft->height() );
	PianoRoll::clear();
}

#endif