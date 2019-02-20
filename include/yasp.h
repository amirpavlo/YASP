/*
  Copyright (c) 2019 Amir Shehata

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#ifndef SPEECH_PARSER_H
#define SPEECH_PARSER_H

#include <err.h>

struct yasp_word {
	struct list_head ph_on_list;
	char *ph_word;
	int ph_start;
	int ph_end;
	int ph_duration;
	float64 ph_prob;
	int32 ph_lscr;
	int32 ph_ascr;
	int32 ph_lback;
};

struct yasp_logs {
	FILE *lg_error;
	FILE *lg_info;
};

/*
 * yasp_interpret_hypothesis
 *	interpret speech clip and return a list of words and times
 */
int yasp_interpret_hypothesis(const char *faudio, const char *ftranscript,
			      const char *genpath,
			      struct list_head *word_list);

/*
 * yasp_interpret_phonemes
 *	interpret speech clip and return a list of phonemes and times
 */
int yasp_interpret_phonemes(const char *faudio, const char *ftranscript,
			    const char *genpath,
			    struct list_head *phoneme_list);

/*
 * yasp_interpret
 *	interpret speech and write json file
 */
int yasp_interpret(const char *audioFile, const char *transcript,
		   const char *output, const char *genpath);

/*
 * yasp_interpret_get_str
 *	interpret speech and get a json string
 */
char *yasp_interpret_get_str(const char *audioFile, const char *transcript,
			     const char *genpath);

/*
 * yasp_free_json_str
 *	Free the JSON string returned in yasp_interpret_get_str()
 */
void yasp_free_json_str(char *json);

/*
 * yasp_interpret_breadown
 *	interpret script and return the word and phoneme list
 */
int yasp_interpret_breadown(const char *audioFile, const char *transcript,
			    const char *output, const char *genpath,
			    struct list_head *word_list,
			    struct list_head *phoneme_list);

/*
 * yasp_parse_transcript
 *	parse a text file pointed at by fh into a list
 */
int yasp_parse_transcript(struct list_head *transcript, FILE *fh);

/*
 * yasp_create_json
 * yasp_create_json_file
 *	yasp_create_json() returns a json string of the word and
 *	phoneme list
 *	The assumption here is the two lists have been consolidated time
 *	wise. IE the timing matches
 *	yasp_create_json_file() writes the output to the given path
 */
char *yasp_create_json(struct list_head *word_list,
		       struct list_head *phoneme_list);

int yasp_create_json_file(struct list_head *word_list,
			  struct list_head *phoneme_list,
			  const char *output);

/*
 * yasp_log
 *	logging function
 */
void yasp_log(void *user_data, err_lvl_t el, const char *fmt, ...);

/*
 * yasp_pprint_segment_list
 *	pretty print a provided segment list
 */
void yasp_pprint_segment_list(struct list_head *seg_list);

/*
 * yasp_print_segment_list
 *	print a provided segment list. Good for taking the output and
 *	pasting it in a spreadsheet, using space as delimiter.
 */
void yasp_print_segment_list(struct list_head *seg_list);

/*
 * yasp_free_segment_list
 *	Free a segment list
 */
void yasp_free_segment_list(struct list_head *seg_list);

/*
 * Setup logging to a log file
 *	cb: if NULL yasp_log is used
 * Finalize log files.
 */
void yasp_setup_logging(struct yasp_logs *logs, err_cb_f cb,
			const char *logfile);
void yasp_finish_logging(struct yasp_logs *logs);

/* explicitly set the model directory */
void yasp_set_modeldir(const char *modeldir);
#endif /* SPEECH_PARSER_H */
