/* 
 * Asterisk-Sphinx Generic Speech API Engine
 *
 * Plugin to communicate and decode speech via CMU Sphinx-based server.
 *
 * Copyright (C) 2009, Christopher Jansen
 * 
 * Chris Jansen <scribblej@scribblej.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */
/*! \file
 * \brief Sphinx integration header, not particularly interesting.
 */

#ifndef _ASTERISK_SPEECH_SPHINX_H
#define _ASTERISK_SPEECH_SPHINX_H


/*! 
 * \brief Create Sphinx module
 * \param speech Speech API object
 * \param format SFORMAT indicator, is always SLINEAR
 *
 * This is called from within Aterisk, you do not need it.
 */
int sphinx_create(struct ast_speech *speech, int format);

/*! 
 * \brief Destroy Sphinx module
 * \param speech Speech API object
 *
 * This is called from within Aterisk, you do not need it.
 */
int sphinx_destroy(struct ast_speech *speech);

/*! 
 * \brief Stub Function
 * \param speech Speech API object
 * \param grammar_name Name of grammar to load
 * \param grammar No clue.
 *
 * These are stub functions that always return true, only included to be 
 * sure of source-level compatibility with other engines.  Load the grammars
 * in advance on the server side.
 */
int sphinx_load(struct ast_speech *speech, char *grammar_name, char *grammar);

/*! 
 * \brief Load / Unload grammar
 * \param speech Speech API object
 * \param grammar_name Name of grammar to load
 *
 * These are stub functions that always return true, only included to be 
 * sure of source-level compatibility with other engines.  Load the grammars
 * in advance on the server side.
 */
int sphinx_unload(struct ast_speech *speech, char *grammar_name);

/*! 
 * \brief Activate grammar
 * \param speech Speech API object
 * \param grammar_name Name of grammar to activate
 *
 * Unlike loading a grammar, activating the grammar is critical; this determines
 * which set of words the sphinx server will be listening for.
 */
int sphinx_activate(struct ast_speech *speech, char *grammar_name);

/*! 
 * \brief Deactivate grammar / wrap up processing
 * \param speech Speech API object
 * \param grammar_name Name of grammar to deactivate
 *
 * This does not actually deactivate the current grammar, but if you make sure it is
 * called before looking for results (as in typical operation) this does signal to
 * Sphinx server that it is the last chance to provide final results.  A good idea.
 */
int sphinx_deactivate(struct ast_speech *speech, char *grammar_name);

/*! 
 * \brief Write audio data to Sphinx server
 * \param speech Speech API object
 * \param data pointer to audio data
 * \param len length of audio data
 *
 * The meat of the operation; data must be in SLINEAR format.
 */
int sphinx_write(struct ast_speech *speech, void *data, int len);

/*! 
 * \brief Stub Function
 * \param speech Speech API object
 *
 * These are stub functions that always return true, only included to be 
 * sure of source-level compatibility with other engines. 
 */
int sphinx_dtmf(struct ast_speech *speech, const char *dtmf);

/*! 
 * \brief Start speech recognition
 * \param speech Speech API object
 *
 * Basically exists to be called before sending data with sphinx_write.
 */
int sphinx_start(struct ast_speech *speech);

/*! 
 * \brief Stub Function
 * \param speech Speech API object
 *
 * These are stub functions that always return true, only included to be 
 * sure of source-level compatibility with other engines. 
 */
int sphinx_change(struct ast_speech *speech, char *name, const char *value);

/*! 
 * \brief Stub Function
 * \param speech Speech API object
 *
 * These are stub functions that always return true, only included to be 
 * sure of source-level compatibility with other engines. 
 */
int sphinx_change_results_type(struct ast_speech *speech,
							   enum ast_speech_results_type results_type);

/*! 
 * \brief Get results
 * \param speech Speech API object
 *
 * Returns pointer to results of speech recognition, NULL if none.
 */
struct ast_speech_result *sphinx_get(struct ast_speech *speech);

/*! \brief 
 * Stores sphinx engine instance state. 
 *
 */


struct sphinx_state {
	int s;						  /* Socket connection to Sphinx Server */
	int heardspeech;		/* True if we have detected speech */
	int noiseframes;		/* Number of consecutive non-silent frames */
	int final;					/* True if we have recieved final results */
	struct ast_dsp *dsp;/* Holds our silence-detection DSP */
	char *sbuf;					/* Data pending to send */
	char *rbuf;					/* Data pending to read */
	int preads;					/* Number of outstanding requests */
	int prbytes;				/* Bytes pending within a request */
	int rbufused;				/* How full is rbuf? */
	int pwbytes;				/* Bytes pending to write */
};

/*! \brief
 *
 * Client and Server do not use the same header, but it's critical this structure is
 * identical between them.
 *
 */
enum e_reqtype {
	REQTYPE_GRAMMAR,
	REQTYPE_START,
	REQTYPE_DATA,
	REQTYPE_FINISH
};

/*! \brief
 *
 * The packet we send to the Sphinx server
 *
 */
struct sphinx_request {
	int dlen;
	enum e_reqtype rtype;
	char *data;
};

#endif /* _ASTERISK_SPEECH_SPHINX_H */
