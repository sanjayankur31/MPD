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

#include "config.h"
#include "ShoutOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "../Wrapper.hxx"
#include "encoder/EncoderInterface.hxx"
#include "encoder/EncoderPlugin.hxx"
#include "encoder/EncoderList.hxx"
#include "config/ConfigError.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "system/FatalError.hxx"
#include "Log.hxx"

#include <shout/shout.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static constexpr unsigned DEFAULT_CONN_TIMEOUT = 2;

struct ShoutOutput final {
	AudioOutput base;

	shout_t *shout_conn;
	shout_metadata_t *shout_meta;

	PreparedEncoder *prepared_encoder = nullptr;
	Encoder *encoder;

	float quality = -2.0;
	int bitrate = -1;

	int timeout = DEFAULT_CONN_TIMEOUT;

	uint8_t buffer[32768];

	ShoutOutput()
		:base(shout_output_plugin),
		 shout_conn(shout_new()),
		 shout_meta(shout_metadata_new()) {}

	~ShoutOutput();

	bool Initialize(const ConfigBlock &block, Error &error) {
		return base.Configure(block, error);
	}

	bool Configure(const ConfigBlock &block, Error &error);
	static ShoutOutput *Create(const ConfigBlock &block, Error &error);

	bool Open(AudioFormat &audio_format, Error &error);
	void Close();

	unsigned Delay() const;
	void SendTag(const Tag &tag);
	size_t Play(const void *chunk, size_t size, Error &error);
	void Cancel();
	bool Pause();
};

static int shout_init_count;

static constexpr Domain shout_output_domain("shout_output");

ShoutOutput::~ShoutOutput()
{
	if (shout_meta != nullptr)
		shout_metadata_free(shout_meta);
	if (shout_conn != nullptr)
		shout_free(shout_conn);

	shout_init_count--;
	if (shout_init_count == 0)
		shout_shutdown();

	delete prepared_encoder;
}

static const EncoderPlugin *
shout_encoder_plugin_get(const char *name)
{
	if (strcmp(name, "ogg") == 0)
		name = "vorbis";
	else if (strcmp(name, "mp3") == 0)
		name = "lame";

	return encoder_plugin_get(name);
}

gcc_pure
static const char *
require_block_string(const ConfigBlock &block, const char *name)
{
	const char *value = block.GetBlockValue(name);
	if (value == nullptr)
		FormatFatalError("no \"%s\" defined for shout device defined "
				 "at line %d\n", name, block.line);

	return value;
}

inline bool
ShoutOutput::Configure(const ConfigBlock &block, Error &error)
{

	const AudioFormat audio_format = base.config_audio_format;
	if (!audio_format.IsFullyDefined()) {
		error.Set(config_domain,
			  "Need full audio format specification");
		return false;
	}

	const char *host = require_block_string(block, "host");
	const char *mount = require_block_string(block, "mount");
	unsigned port = block.GetBlockValue("port", 0u);
	if (port == 0) {
		error.Set(config_domain, "shout port must be configured");
		return false;
	}

	const char *passwd = require_block_string(block, "password");
	const char *name = require_block_string(block, "name");

	bool is_public = block.GetBlockValue("public", false);

	const char *user = block.GetBlockValue("user", "source");

	const char *value = block.GetBlockValue("quality");
	if (value != nullptr) {
		char *test;
		quality = strtod(value, &test);

		if (*test != '\0' || quality < -1.0 || quality > 10.0) {
			error.Format(config_domain,
				     "shout quality \"%s\" is not a number in the "
				     "range -1 to 10",
				     value);
			return false;
		}

		if (block.GetBlockValue("bitrate") != nullptr) {
			error.Set(config_domain,
				  "quality and bitrate are "
				  "both defined");
			return false;
		}
	} else {
		value = block.GetBlockValue("bitrate");
		if (value == nullptr) {
			error.Set(config_domain,
				  "neither bitrate nor quality defined");
			return false;
		}

		char *test;
		bitrate = strtol(value, &test, 10);

		if (*test != '\0' || bitrate <= 0) {
			error.Set(config_domain,
				  "bitrate must be a positive integer");
			return false;
		}
	}

	const char *encoding = block.GetBlockValue("encoding", "ogg");
	const auto encoder_plugin = shout_encoder_plugin_get(encoding);
	if (encoder_plugin == nullptr) {
		error.Format(config_domain,
			     "couldn't find shout encoder plugin \"%s\"",
			     encoding);
		return false;
	}

	prepared_encoder = encoder_init(*encoder_plugin, block, error);
	if (prepared_encoder == nullptr)
		return false;

	unsigned shout_format;
	if (strcmp(encoding, "mp3") == 0 || strcmp(encoding, "lame") == 0)
		shout_format = SHOUT_FORMAT_MP3;
	else
		shout_format = SHOUT_FORMAT_OGG;

	unsigned protocol;
	value = block.GetBlockValue("protocol");
	if (value != nullptr) {
		if (0 == strcmp(value, "shoutcast") &&
		    0 != strcmp(encoding, "mp3")) {
			error.Format(config_domain,
				     "you cannot stream \"%s\" to shoutcast, use mp3",
				     encoding);
			return false;
		} else if (0 == strcmp(value, "shoutcast"))
			protocol = SHOUT_PROTOCOL_ICY;
		else if (0 == strcmp(value, "icecast1"))
			protocol = SHOUT_PROTOCOL_XAUDIOCAST;
		else if (0 == strcmp(value, "icecast2"))
			protocol = SHOUT_PROTOCOL_HTTP;
		else {
			error.Format(config_domain,
				     "shout protocol \"%s\" is not \"shoutcast\" or "
				     "\"icecast1\"or \"icecast2\"",
				     value);
			return false;
		}
	} else {
		protocol = SHOUT_PROTOCOL_HTTP;
	}

	if (shout_set_host(shout_conn, host) != SHOUTERR_SUCCESS ||
	    shout_set_port(shout_conn, port) != SHOUTERR_SUCCESS ||
	    shout_set_password(shout_conn, passwd) != SHOUTERR_SUCCESS ||
	    shout_set_mount(shout_conn, mount) != SHOUTERR_SUCCESS ||
	    shout_set_name(shout_conn, name) != SHOUTERR_SUCCESS ||
	    shout_set_user(shout_conn, user) != SHOUTERR_SUCCESS ||
	    shout_set_public(shout_conn, is_public) != SHOUTERR_SUCCESS ||
	    shout_set_format(shout_conn, shout_format)
	    != SHOUTERR_SUCCESS ||
	    shout_set_protocol(shout_conn, protocol) != SHOUTERR_SUCCESS ||
	    shout_set_agent(shout_conn, "MPD") != SHOUTERR_SUCCESS) {
		error.Set(shout_output_domain, shout_get_error(shout_conn));
		return false;
	}

	/* optional paramters */
	timeout = block.GetBlockValue("timeout", DEFAULT_CONN_TIMEOUT);

	value = block.GetBlockValue("genre");
	if (value != nullptr && shout_set_genre(shout_conn, value)) {
		error.Set(shout_output_domain, shout_get_error(shout_conn));
		return false;
	}

	value = block.GetBlockValue("description");
	if (value != nullptr && shout_set_description(shout_conn, value)) {
		error.Set(shout_output_domain, shout_get_error(shout_conn));
		return false;
	}

	value = block.GetBlockValue("url");
	if (value != nullptr && shout_set_url(shout_conn, value)) {
		error.Set(shout_output_domain, shout_get_error(shout_conn));
		return false;
	}

	{
		char temp[11];

		snprintf(temp, sizeof(temp), "%u", audio_format.channels);
		shout_set_audio_info(shout_conn, SHOUT_AI_CHANNELS, temp);

		snprintf(temp, sizeof(temp), "%u", audio_format.sample_rate);

		shout_set_audio_info(shout_conn, SHOUT_AI_SAMPLERATE, temp);

		if (quality >= -1.0) {
			snprintf(temp, sizeof(temp), "%2.2f", quality);
			shout_set_audio_info(shout_conn, SHOUT_AI_QUALITY,
					     temp);
		} else {
			snprintf(temp, sizeof(temp), "%d", bitrate);
			shout_set_audio_info(shout_conn, SHOUT_AI_BITRATE,
					     temp);
		}
	}

	return true;
}

ShoutOutput *
ShoutOutput::Create(const ConfigBlock &block, Error &error)
{
	if (shout_init_count == 0)
		shout_init();

	shout_init_count++;

	ShoutOutput *sd = new ShoutOutput();
	if (!sd->Initialize(block, error)) {
		delete sd;
		return nullptr;
	}

	if (!sd->Configure(block, error)) {
		delete sd;
		return nullptr;
	}

	return sd;
}

static bool
handle_shout_error(ShoutOutput *sd, int err, Error &error)
{
	switch (err) {
	case SHOUTERR_SUCCESS:
		break;

	case SHOUTERR_UNCONNECTED:
	case SHOUTERR_SOCKET:
		error.Format(shout_output_domain, err,
			     "Lost shout connection to %s:%i: %s",
			     shout_get_host(sd->shout_conn),
			     shout_get_port(sd->shout_conn),
			     shout_get_error(sd->shout_conn));
		return false;

	default:
		error.Format(shout_output_domain, err,
			     "connection to %s:%i error: %s",
			     shout_get_host(sd->shout_conn),
			     shout_get_port(sd->shout_conn),
			     shout_get_error(sd->shout_conn));
		return false;
	}

	return true;
}

static bool
write_page(ShoutOutput *sd, Error &error)
{
	assert(sd->encoder != nullptr);

	while (true) {
		size_t nbytes = sd->encoder->Read(sd->buffer,
						  sizeof(sd->buffer));
		if (nbytes == 0)
			return true;

		int err = shout_send(sd->shout_conn, sd->buffer, nbytes);
		if (!handle_shout_error(sd, err, error))
			return false;
	}

	return true;
}

void
ShoutOutput::Close()
{
	if (encoder != nullptr) {
		if (encoder->End(IgnoreError()))
			write_page(this, IgnoreError());

		delete encoder;
	}

	if (shout_get_connected(shout_conn) != SHOUTERR_UNCONNECTED &&
	    shout_close(shout_conn) != SHOUTERR_SUCCESS) {
		FormatWarning(shout_output_domain,
			      "problem closing connection to shout server: %s",
			      shout_get_error(shout_conn));
	}
}

void
ShoutOutput::Cancel()
{
	/* needs to be implemented for shout */
}

static bool
shout_connect(ShoutOutput *sd, Error &error)
{
	switch (shout_open(sd->shout_conn)) {
	case SHOUTERR_SUCCESS:
	case SHOUTERR_CONNECTED:
		return true;

	default:
		error.Format(shout_output_domain,
			     "problem opening connection to shout server %s:%i: %s",
			     shout_get_host(sd->shout_conn),
			     shout_get_port(sd->shout_conn),
			     shout_get_error(sd->shout_conn));
		return false;
	}
}

bool
ShoutOutput::Open(AudioFormat &audio_format, Error &error)
{
	if (!shout_connect(this, error))
		return false;

	encoder = prepared_encoder->Open(audio_format, error);
	if (encoder == nullptr) {
		shout_close(shout_conn);
		return false;
	}

	if (!write_page(this, error)) {
		delete encoder;
		shout_close(shout_conn);
		return false;
	}

	return true;
}

unsigned
ShoutOutput::Delay() const
{
	int delay = shout_delay(shout_conn);
	if (delay < 0)
		delay = 0;

	return delay;
}

size_t
ShoutOutput::Play(const void *chunk, size_t size, Error &error)
{
	return encoder->Write(chunk, size, error) &&
		write_page(this, error)
		? size
		: 0;
}

bool
ShoutOutput::Pause()
{
	static char silence[1020];

	return Play(silence, sizeof(silence), IgnoreError());
}

static void
shout_tag_to_metadata(const Tag &tag, char *dest, size_t size)
{
	char artist[size];
	char title[size];

	artist[0] = 0;
	title[0] = 0;

	for (const auto &item : tag) {
		switch (item.type) {
		case TAG_ARTIST:
			strncpy(artist, item.value, size);
			break;
		case TAG_TITLE:
			strncpy(title, item.value, size);
			break;

		default:
			break;
		}
	}

	snprintf(dest, size, "%s - %s", artist, title);
}

void
ShoutOutput::SendTag(const Tag &tag)
{
	if (encoder->ImplementsTag()) {
		/* encoder plugin supports stream tags */

		Error error;
		if (!encoder->PreTag(error) ||
		    !write_page(this, error) ||
		    !encoder->SendTag(tag, error)) {
			LogError(error);
			return;
		}
	} else {
		/* no stream tag support: fall back to icy-metadata */
		char song[1024];
		shout_tag_to_metadata(tag, song, sizeof(song));

		shout_metadata_add(shout_meta, "song", song);
		if (SHOUTERR_SUCCESS != shout_set_metadata(shout_conn,
							   shout_meta)) {
			LogWarning(shout_output_domain,
				   "error setting shout metadata");
		}
	}

	write_page(this, IgnoreError());
}

typedef AudioOutputWrapper<ShoutOutput> Wrapper;

const struct AudioOutputPlugin shout_output_plugin = {
	"shout",
	nullptr,
	&Wrapper::Init,
	&Wrapper::Finish,
	nullptr,
	nullptr,
	&Wrapper::Open,
	&Wrapper::Close,
	&Wrapper::Delay,
	&Wrapper::SendTag,
	&Wrapper::Play,
	nullptr,
	&Wrapper::Cancel,
	&Wrapper::Pause,
	nullptr,
};
