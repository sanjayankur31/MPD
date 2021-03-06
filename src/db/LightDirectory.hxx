/*
 * Copyright 2003-2016 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_LIGHT_DIRECTORY_HXX
#define MPD_LIGHT_DIRECTORY_HXX

#include "Compiler.h"

#include <string>

#include <time.h>

struct Tag;

/**
 * A reference to a directory.  Unlike the #Directory class, this one
 * consists only of pointers.  It is supposed to be as light as
 * possible while still providing all the information MPD has about a
 * directory.  This class does not manage any memory, and the pointers
 * become invalid quickly.  Only to be used to pass around during
 * well-defined situations.
 */
struct LightDirectory {
	const char *uri;

	time_t mtime;

	constexpr LightDirectory(const char *_uri, time_t _mtime)
		:uri(_uri), mtime(_mtime) {}

	static constexpr LightDirectory Root() {
		return LightDirectory("", 0);
	}

	bool IsRoot() const {
		return *uri == 0;
	}

	gcc_pure
	const char *GetPath() const {
		return uri;
	}
};

#endif
