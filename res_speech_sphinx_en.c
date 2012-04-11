/* 
 * Asterisk-Sphinx Generic Speech API Module
 *
 * Copyright (C) 2009, Christopher Jansen
 *
 * Christopher Jansen, <scribblej@scribblej.com>
 *
 * Generic Speech Engine plugin that provides CMU Sphinx as a 
 * speech recognition possibility for Asterisk.  Requires the
 * astsphinx server to communicate with, distributed separately.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. 
 *
 */

/*! \file
 *
 * \brief Sphinx Generix Speech Recognition API Plugin
 *
 * \author Christopher Jansen <scribblej@scribblej.com>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: $")


#include <asterisk/module.h>
#include <asterisk/logger.h>
#include <asterisk/strings.h>
#include <asterisk/config.h>
#include <asterisk/frame.h>
#include <asterisk/dsp.h>
#include <asterisk/speech.h>
#include "speech_sphinx.h"

/* Not sure how to handle TCP socket in *, so... */
#include <sys/poll.h>
#include <sys/select.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>


 
/*! \brief SPHINX_BUFZISE is heap-allocated buffer for comms */

#define AST_MODULE "speech_sphinx_en"
#define SPHINX_BUFSIZE 2048
#define SPHINX_ERROR   0
#define SPHINX_SUCCESS 1

/* Functions used internally only */
/*! \brief Logs the current state as a NOTICE */
	 void log_state(struct ast_speech *speech);
/*! \brief Establish socket connection to sphinx server */
	 int sphinx_connect(struct ast_speech *speech, char const *host, const int port);
/*! \brief Disconnect socket */
	 int sphinx_disconnect(struct ast_speech *speech);
/*! \brief exchange packets with server */
	 int sphinx_comm(struct sphinx_request *sr, struct ast_speech *speech, int catchup);
/*! \brief clear all data */
	 int reinit_speech_data(struct ast_speech *speech);
/*! \brief destroy all data */
	 int destroy_speech_data(struct ast_speech *speech);
/*! \brief set socket blocking mode */
	 int sphinx_set_blocking(int s, int shouldblock);
/*! \brief write raw data to socket */
	 int sphinx_swrite(struct sphinx_state *ss, void *data, int len);
/*! \brief read raw data from socket */
	 int sphinx_sread(struct sphinx_state *ss, struct ast_speech *speech);
/*! \brief Change state and log error */
	 int make_error(struct ast_speech *speech, char *errmsg);

/*! \brief API description */
	 static struct ast_speech_engine SPHINX_ENGINE_INFO = 
	       { "Sphinx-En",
		 sphinx_create,
		 sphinx_destroy,
		 sphinx_load,
		 sphinx_unload,
		 sphinx_activate,
		 sphinx_deactivate,
		 sphinx_write,
		 sphinx_dtmf,
		 sphinx_start,
		 sphinx_change,
		 sphinx_change_results_type,
		 sphinx_get,
		 AST_FORMAT_SLINEAR
	 };

/*! \brief Global settings */
char SPHINX_SERVER_ADDR[] = "127.000.000.001"; /* Sloppy? */
int SPHINX_SERVER_PORT = 10070;
int SPHINX_SILENCE_TIME = 200;
int SPHINX_NOISE_FRAMES = 0;
int SPHINX_SILENCE_THRESHOLD = 500;


/*! \brief set socket blocking mode */
int sphinx_set_blocking(int s, int shouldblock)
{
	int block = shouldblock ? 0 : O_NONBLOCK;
	int sflags;

	if ((sflags = fcntl(s, F_GETFL, 0)) == -1)
		return SPHINX_ERROR;
	if (fcntl(s, F_SETFL, sflags | block) == -1)
		return SPHINX_ERROR;
	return SPHINX_SUCCESS;
}

/*! \brief load module, config settings */
static int load_module(void)
{
	struct ast_flags config_flags = { 0 };
	struct ast_config *conf = ast_config_load("sphinx_en.conf", config_flags);
	char const *value;

	if (conf == NULL) {
		ast_log(LOG_ERROR, "Unable to load sphinx.conf\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	if ((value = ast_variable_retrieve(conf, "general", "serverip"))) {
		ast_copy_string(SPHINX_SERVER_ADDR, value, sizeof(SPHINX_SERVER_ADDR));
	}
	if ((value = ast_variable_retrieve(conf, "general", "serverport"))) {
		sscanf(value, "%d", &SPHINX_SERVER_PORT);
	}
	if ((value = ast_variable_retrieve(conf, "general", "silencetime"))) {
		sscanf(value, "%d", &SPHINX_SILENCE_TIME);
	}
	if ((value = ast_variable_retrieve(conf, "general", "noiseframes"))) {
		sscanf(value, "%d", &SPHINX_NOISE_FRAMES);
	}
	if ((value = ast_variable_retrieve(conf, "general", "silencethreshold"))) {
		sscanf(value, "%d", &SPHINX_SILENCE_THRESHOLD);
	}

	ast_log(LOG_NOTICE,
			"Using Server: %s:%d Silence Time: %d Threshold: %d Noise Frames: %d\n",
			SPHINX_SERVER_ADDR, SPHINX_SERVER_PORT, SPHINX_SILENCE_TIME,
			SPHINX_SILENCE_THRESHOLD, SPHINX_NOISE_FRAMES);

	if (ast_speech_register(&SPHINX_ENGINE_INFO)) {
		ast_log(LOG_ERROR, "Failed to register.\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

/*! \brief Unload module */
static int unload_module(void)
{
	if (ast_speech_unregister(SPHINX_ENGINE_INFO.name)) {
		ast_log(LOG_ERROR, "Failed to unregister.\n");
		return -1;
	}

	return 0;
}

/*! \brief Create instance of Sphinx engine */
int sphinx_create(struct ast_speech *speech, int format)
{
	/* ast_log(LOG_DEBUG, "sphinx_create called\n"); */
	if (reinit_speech_data(speech) == SPHINX_SUCCESS)
		if (sphinx_connect(speech, SPHINX_SERVER_ADDR, SPHINX_SERVER_PORT) ==
			SPHINX_SUCCESS)
			return 0;

	ast_log(LOG_ERROR, "Can't create Sphinx server\n");
	return -1;
}

/*! \brief Destroy instance of Sphinx engine */
int sphinx_destroy(struct ast_speech *speech)
{
	/* ast_log(LOG_DEBUG, "sphinx_destroy called\n"); */
	if (sphinx_disconnect(speech) == SPHINX_SUCCESS)
		if (destroy_speech_data(speech) == SPHINX_SUCCESS)
			return SPHINX_SUCCESS;

	return SPHINX_ERROR;
}

/*! \brief The goggles, they do nothing! */
int sphinx_load(struct ast_speech *speech, char *grammar_name, char *grammar)
{
  /*  ast_log(LOG_DEBUG, "sphinx_load called with request for grammar %s\n", grammar_name); */
	return 0;
}

/*! \brief The goggles, they do nothing! */
int sphinx_unload(struct ast_speech *speech, char *grammar_name)
{
	/* ast_log(LOG_DEBUG, "sphinx_unload called for grammar %s\n", grammar_name); */
	return 0;
}

/*! \brief Chooses which grammar set to use on Sphinx server (i.e. which words to listen for */
int sphinx_activate(struct ast_speech *speech, char *grammar_name)
{
	struct sphinx_request sr;
	sr.rtype = REQTYPE_GRAMMAR;
	sr.dlen = strlen(grammar_name) + 1;
	sr.data = grammar_name;
	if (sphinx_comm(&sr, speech, 1) != SPHINX_SUCCESS) {
		ast_log(LOG_ERROR, "Comms error changing grammar request\n");
		ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
		return -1;
	}

	ast_speech_change_state(speech, AST_SPEECH_STATE_READY);
	return 0;
}

/*! \brief 
 * sphinx_deactivate does nothing to the grammar but based on the examples
 * I've seen it's a good place to signal to the engine this is its
 * last chance to provide results.
 */
int sphinx_deactivate(struct ast_speech *speech, char *grammar_name)
{
	struct sphinx_state *ss = (struct sphinx_state *) speech->data;
	if (ss == NULL) {
		ast_log(LOG_ERROR, "Error, no state\n");
		return -1;
	}

	if (speech->state != AST_SPEECH_STATE_DONE && !ss->final) {
		if (sphinx_write(speech, NULL, 0) != 0) {
			ast_log(LOG_ERROR, "Comms error - setting NOT_READY\n");
			ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
			return -1;
		}
	}

	return 0;
}

/*! \brief non-blocking data read from socket */
int sphinx_sread(struct sphinx_state *ss, struct ast_speech *speech)
{
	int rbytes = 0;

	if (ss->preads == 0 && ss->prbytes == 0) {
		return SPHINX_SUCCESS;
	}

	while (ss->preads && !ss->prbytes) {
		int32_t rsize = 0;
		if ((rbytes = read(ss->s, &rsize, sizeof(int32_t))) == -1) {
			if (errno != EWOULDBLOCK) {
				return make_error(speech, strerror(errno));
			} else {
				break;
			}
		} else if (rbytes == sizeof(int32_t)) {
			ss->rbufused = 0;
			ss->prbytes = rsize;
			ss->preads--;
		}
	}

	if (ss->prbytes + ss->rbufused > SPHINX_BUFSIZE)
		return make_error(speech, "BUFFER OVERFLOW IN SPHINX READ BUFFER\n");

	rbytes = 0;
	while (ss->prbytes != 0 &&
		   (rbytes = read(ss->s, ss->rbuf + ss->rbufused, ss->prbytes)) != -1) {
		if (rbytes != 0) {
			ss->prbytes -= rbytes;
			ss->rbufused += rbytes;
		}
	}

	if (rbytes == -1) {
		if (errno == EWOULDBLOCK) {
			rbytes = 0;
		} else {
			return make_error(speech, strerror(errno));
		}
	}


	if (rbytes && !ss->prbytes)	/* We finished reading some results. */
	{
    int32_t new_score = 0;
		if (speech->results == NULL)
			speech->results = ast_calloc(sizeof(struct ast_speech_result), 1);
		if (speech->results == NULL)
			return make_error(speech, "Cannot allocate results\n");

		new_score = *(int32_t *) ss->rbuf;
		if (new_score >= speech->results->score) {
			speech->results->score = new_score;
			if (speech->results->text != NULL) {
				free(speech->results->text);
				speech->results->text = NULL;
			}
			speech->results->text =
				ast_strndup(ss->rbuf + sizeof(int32_t), ss->rbufused - sizeof(int32_t));
			ast_log(LOG_NOTICE, "Score: %d Result: '%s'\n", speech->results->score,
					speech->results->text);
		} else {
			ast_log(LOG_NOTICE, "New result with lower score; ignoring.\n");
		}
		speech->flags |= AST_SPEECH_HAVE_RESULTS;
	}

	return SPHINX_SUCCESS;

}

/*! \brief Log an error, set error state. */
int make_error(struct ast_speech *speech, char *errmsg)
{
	ast_log(LOG_ERROR, "%s", errmsg);
	ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
	return SPHINX_ERROR;
}

/*! \brief non-blocking socket write */
int sphinx_swrite(struct sphinx_state *ss, void *indata, int len)
{
	char *data = (char *) indata;

	if (ss->pwbytes + len > SPHINX_BUFSIZE)	// Too much data!
	{
		ast_log(LOG_ERROR, "Output buffer overflow.\n");
		return SPHINX_ERROR;
	}

	memcpy(ss->sbuf + ss->pwbytes, data, len);
	ss->pwbytes += len;

	if (ss->pwbytes) /* Something to send */
	{
		int bcount = write(ss->s, ss->sbuf, ss->pwbytes);
		if (bcount == -1 && (errno != EWOULDBLOCK)) {
			ast_log(LOG_ERROR, "Error writing to Sphinx server: %s\n", strerror(errno));
			return SPHINX_ERROR;
		}
		if (bcount == -1)
			bcount = 0;
		memmove(ss->sbuf, ss->sbuf + bcount, ss->pwbytes - bcount);
		ss->pwbytes -= bcount;
	}
	return SPHINX_SUCCESS;
}



/*! \brief
 * Does the 'big job' of getting data to/from the Sphinx Server.  The comm protocol
 * if pretty stupid simple; we send a two-int header consisting of the length
 * of data to follow, then the type of request we are sending, then the data.  We
 * expect a similar response, only without the type of request.
 */
int sphinx_comm(struct sphinx_request *sr, struct ast_speech *speech, int catchup)
{
	if (speech == NULL)
		return make_error(speech, "No data\n");
	struct sphinx_state *ss = (struct sphinx_state *) speech->data;

	if (ss == NULL)
		return make_error(speech, "No state\n");
	if (ss->s == 0)
		return make_error(speech, "No socket\n");
	if (sr == NULL)
		return make_error(speech, "No request\n");

	if ((sr->rtype & (REQTYPE_FINISH | REQTYPE_DATA)) &&
		(speech->state == AST_SPEECH_STATE_DONE || ss->final)) {
		sr->dlen = 0;
	} else {
		/* Write request type, data length */
		if (sphinx_swrite(ss, &sr->dlen, sizeof(sr->dlen)) != SPHINX_SUCCESS)
			return make_error(speech, "Socket write error sending dlen\n");

		if (sphinx_swrite(ss, &sr->rtype, sizeof(sr->rtype)) != SPHINX_SUCCESS)
			return make_error(speech, "Socket write error sending rtype\n");

		/* Write actual data, if any */
		if (sr->dlen) {
			if (sphinx_swrite(ss, sr->data, sr->dlen) != SPHINX_SUCCESS)
				return make_error(speech, "Socket write error sending data\n");
		}

		ss->preads++;			/* Increment count of pending responses to expect */

    /* If we sent nothing, this is also a signal to finish */
		if ((sr->rtype == REQTYPE_DATA && sr->dlen == 0) || sr->rtype == REQTYPE_FINISH)
		{
			ast_speech_change_state(speech, AST_SPEECH_STATE_DONE);
			ss->final = 1;
		}

	}

	/* ok, so we need a chance to read some data, and here it is.  Normally, we just want to call read and it'll do what
	 * it can, but if we are final, then we gotta make sure it finishes up.
   */
	if (sphinx_sread(ss, speech) != SPHINX_SUCCESS)
		return SPHINX_ERROR;

	if (speech->state == AST_SPEECH_STATE_DONE || ss->final || catchup) {
		while (ss->preads || ss->prbytes || ss->pwbytes) {
			/* ast_log(LOG_NOTICE, "Flushing buffers, Responses Pending: %d, Bytes in current response: %d, Bytes to write: %d\n",
			 *       ss->preads, ss->prbytes, ss->pwbytes);
       */

			fd_set rsel, wsel;
			struct timeval tv;
			int selret;

			FD_ZERO(&rsel);
			FD_ZERO(&wsel);
			if (ss->pwbytes)
				FD_SET(ss->s, &wsel);
			if (ss->prbytes || ss->preads)
				FD_SET(ss->s, &rsel);

			/* 5 seconds timeout is extreme. */
			tv.tv_sec = 5;
			tv.tv_usec = 0;

			selret = select(ss->s + 1, &rsel, &wsel, NULL, &tv);

			if (selret == -1)
				return make_error(speech, "Select returned error.\n");
			else if (selret == 0) {
				return make_error(speech,
								  "Reached 5-second timeout on socket flush, WTF.\n");
			}

			if (FD_ISSET(ss->s, &wsel)) {
				if (sphinx_swrite(ss, NULL, 0) != SPHINX_SUCCESS)
					return make_error(speech, "Error flushing write buffer.\n");
			}

			if (FD_ISSET(ss->s, &rsel)) {
				if (sphinx_sread(ss, speech) != SPHINX_SUCCESS)
					return make_error(speech, "Error flushing read buffer.\n");
			}
		}
	}

	return SPHINX_SUCCESS;

}

int sphinx_write(struct ast_speech *speech, void *data, int len)
{
	struct ast_frame f;
  int totalsil;
	int silence;
	struct sphinx_request sr;
  int finish;

	if (speech->data == NULL) {
		ast_log(LOG_ERROR, "Socket data does not exist.\n");
		ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
		return -1;
	}
	struct sphinx_state *ss = (struct sphinx_state *) speech->data;

	if (speech->state == AST_SPEECH_STATE_DONE || ss->final) {
		len = 0;
	}

	if (ss->s == 0) {
		ast_log(LOG_ERROR, "Socket does not exist.\n");
		ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
		return -1;
	}

	/* The Sphinx system doesn't seem be helpful in detecting silence and determing
	 * the end of an utterance on its own, so here we use Asterisk's silence detection
	 * DSP to fake sane behaviour. 
	 * The Asterisk Generic Speech API strips the frame away from the data we are
	 * sent, so to use the DSP, here we must re-create a frame.
	 */
	f.data.ptr = data;
	f.datalen = len;
	f.samples = len / 2;
	f.mallocd = 0;
	f.frametype = AST_FRAME_VOICE;
	f.subclass.codec = AST_FORMAT_SLINEAR;

	silence = ast_dsp_silence(ss->dsp, &f, &totalsil);
	/* ast_log(LOG_NOTICE, "DETECT SILENCE: %s, %06d ms\n", silence ? "true" : "false", totalsil); */

	if (!ss->heardspeech && !silence) {
		ss->noiseframes++;
		if (ss->noiseframes > SPHINX_NOISE_FRAMES) {
			/* ast_log(LOG_NOTICE, "Detected speech.\n"); */
			ss->heardspeech = 1;
			ss->noiseframes = 0;
			speech->flags |= AST_SPEECH_QUIET;
			speech->flags |= AST_SPEECH_SPOKE;
		}
	} else if (ss->heardspeech && silence && totalsil > SPHINX_SILENCE_TIME) {
		/* ast_log(LOG_NOTICE, "Detected %d finishing silence.\n", totalsil); */
		/* sending 0 bytes in a DATA request is another way to wrap-up. */
		len = 0;
	} else if (silence)
		ss->noiseframes = 0;

	sr.dlen = len;
	sr.rtype = REQTYPE_DATA;
	sr.data = data;

	finish = 0;
	if (sr.dlen == 0)
		finish = 1;
	if (sphinx_comm(&sr, speech, finish) != SPHINX_SUCCESS) {
		ast_log(LOG_ERROR, "Comms error, changing state to NOT_READY\n");
		ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
		return -1;
	}

	return 0;

}

/*! \brief Does nothing - stub for compatibility. */
int sphinx_dtmf(struct ast_speech *speech, const char *dtmf)
{
	/* ast_log(LOG_DEBUG, "sphinx_dtmf called with %s\n", dtmf); */
	return 0;
}

/*! brief Prepare to accept speech data (via sphinx_write) */
int sphinx_start(struct ast_speech *speech)
{
	/* ast_log(LOG_DEBUG, "sphinx_start called - changing to ready state\n"); */
	if (reinit_speech_data(speech) != SPHINX_SUCCESS) {
		ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
		ast_log(LOG_ERROR, "Cannot reinit speech object, setting NOT READY\n");
		return -1;
	}
	ast_speech_change_state(speech, AST_SPEECH_STATE_READY);
	return 0;
}

/*! \brief Does nothing - stub for compatibility. */
int sphinx_change(struct ast_speech *speech, char *name, const char *value)
{
	/* ast_log(LOG_DEBUG, "sphinx_change called name %s val %s\n", name, value); */
	return 0;
}

/*! \brief Logs current speech object state */
void log_state(struct ast_speech *speech)
{
	switch (speech->state) {
	case AST_SPEECH_STATE_NOT_READY:
		ast_log(LOG_NOTICE, "STATE IS NOT_READY\n");
		break;
	case AST_SPEECH_STATE_READY:
		ast_log(LOG_NOTICE, "STATE IS READY\n");
		break;
	case AST_SPEECH_STATE_WAIT:
		ast_log(LOG_NOTICE, "STATE IS WAIT\n");
		break;
	case AST_SPEECH_STATE_DONE:
		ast_log(LOG_NOTICE, "STATE IS DONE\n");
		break;
	}
}

/*! \brief Does nothing, return error, exists for compatibility. */
int sphinx_change_results_type(struct ast_speech *speech,
							   enum ast_speech_results_type results_type)
{
	/* ast_log(LOG_DEBUG, "sphinx_change_results_type called\n"); */
	return -1;
}

/*! \brief Returns the current speech results */
struct ast_speech_result *sphinx_get(struct ast_speech *speech)
{
	if (speech != NULL) {
		if (speech->results != NULL) {
			return speech->results;
		}
	}
	// ast_log(LOG_NOTICE, "sphinx_get called but no results to return.\n");
	return NULL;
}

/*! \brief connects to sphinx server */
int sphinx_connect(struct ast_speech *speech, char const *host, const int port)
{
	struct sockaddr_in sin;
	struct hostent *hp;
	struct ast_hostent ahp;
	struct sphinx_state *ss = (struct sphinx_state *) speech->data;

	/* State checking */
	if (speech == NULL)
		return SPHINX_ERROR;
	if (sphinx_disconnect(speech) != SPHINX_SUCCESS)
		return SPHINX_ERROR;
	if (ss == NULL)
		return SPHINX_ERROR;
	if (ss->s != 0) {
		ast_log(LOG_ERROR, "Socket exists.\n");
		return SPHINX_ERROR;
	}

	/* Create socket */
	hp = ast_gethostbyname(host, &ahp);
	if (!hp) {
		ast_log(LOG_ERROR, "Unable to locate host '%s'\n", host);
		return SPHINX_ERROR;
	}
	ss->s = socket(AF_INET, SOCK_STREAM, 0);
	if (ss->s <= 0) {
		ast_log(LOG_ERROR, "Unable to create socket: %s\n", strerror(errno));
		ss->s = 0;
		return SPHINX_ERROR;
	}

	/* Make connection */
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	memcpy(&sin.sin_addr, hp->h_addr, sizeof(sin.sin_addr));
	if (connect(ss->s, (struct sockaddr *) &sin, sizeof(sin)) && (errno != EINPROGRESS)) {
		ast_log(LOG_ERROR, "Connect failed with unexpected error: %s\n", strerror(errno));
		close(ss->s);
		ss->s = 0;
		return SPHINX_ERROR;
	}

	ast_log(LOG_DEBUG, "Connect to %s:%d completed.\n", host, port);

	/* No need to get messy with non-blocking, now that we're connected we'll get that rolling */
	if (sphinx_set_blocking(ss->s, 0) != SPHINX_SUCCESS) {
		close(ss->s);
		ss->s = 0;
		return make_error(speech, "Cannot set blocking mode.\n");
	}

	return SPHINX_SUCCESS;
}

/*! \brief init or re-init object data */
int reinit_speech_data(struct ast_speech *speech)
{
  struct sphinx_state *ss;

	/* ast_log(LOG_DEBUG, "initalizing speech data.\n"); */
	if (speech == NULL)
		return SPHINX_ERROR;
	if (speech->data == NULL) {
		speech->data = ast_calloc(sizeof(struct ast_speech), 1);
		if (speech->data == NULL)
			return SPHINX_ERROR;
	}

	ss = (struct sphinx_state *) speech->data;

	if (ss->dsp != NULL) {
		ast_dsp_free(ss->dsp);
		ss->dsp = NULL;
	}

	if (ss->preads || ss->pwbytes || ss->prbytes) {
		ast_log(LOG_ERROR,
				"Pending reads: %d, bytes in current read: %d - WE DO NOT EXPECT PENDING READS HERE!\n",
				ss->preads, ss->pwbytes);
		/* TODO: handle this case better. */
	}
	ss->heardspeech = 0;
	ss->noiseframes = 0;
	ss->final = 0;
	ss->prbytes = 0;
	ss->pwbytes = 0;
	ss->preads = 0;
	ss->rbufused = 0;
	ss->dsp = ast_dsp_new();
	if (ss->dsp == NULL) {
		ast_log(LOG_ERROR, "Unable to create silence detection DSP\n");
		free(ss);
		speech->data = NULL;
		return SPHINX_ERROR;
	}
	ast_dsp_set_threshold(ss->dsp, SPHINX_SILENCE_THRESHOLD);

	if (ss->rbuf != NULL) {
		free(ss->rbuf);
		ss->rbuf = NULL;
	}
	if (ss->sbuf != NULL) {
		free(ss->sbuf);
		ss->sbuf = NULL;
	}

	ss->rbuf = ast_calloc(SPHINX_BUFSIZE, 1);
	if (ss->rbuf == NULL) {
		ast_dsp_free(ss->dsp);
		ss->dsp = NULL;
		free(ss);
		speech->data = NULL;
		return SPHINX_ERROR;
	}

	ss->sbuf = ast_calloc(SPHINX_BUFSIZE, 1);
	if (ss->sbuf == NULL) {
		ast_dsp_free(ss->dsp);
		ss->dsp = NULL;
		free(ss->rbuf);
		ss->rbuf = NULL;
		free(ss);
		speech->data = NULL;
		return SPHINX_ERROR;
	}

	return SPHINX_SUCCESS;
}

/*! \brief Release all used memory, disconnect socket */
int destroy_speech_data(struct ast_speech *speech)
{
  struct sphinx_state *ss;
	if (speech == NULL)
		return SPHINX_ERROR;
	if (speech->data == NULL)
		return SPHINX_SUCCESS;

	if (sphinx_disconnect(speech) != SPHINX_SUCCESS)
		return SPHINX_ERROR;

	ss = (struct sphinx_state *) speech->data;

	if (ss->rbuf != NULL) {
		free(ss->rbuf);
		ss->rbuf = NULL;
	}
	if (ss->sbuf != NULL) {
		free(ss->sbuf);
		ss->sbuf = NULL;
	}

	if (ss->dsp != NULL) {
		ast_dsp_free(ss->dsp);
		ss->dsp = NULL;
	}
	free(ss);
	speech->data = NULL;
	return SPHINX_SUCCESS;
}

/*! \brief Disconnect socket */
int sphinx_disconnect(struct ast_speech *speech)
{
	struct sphinx_state *ss = (struct sphinx_state *) speech->data;
	if (ss == NULL)
		return SPHINX_SUCCESS;

	if (ss->s != 0) {
		close(ss->s);
	}
	ast_log(LOG_DEBUG, "DISCONNECTED\n");
	return SPHINX_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Sphinx Speech Engine");
