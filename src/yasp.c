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

#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <pocketsphinx.h>
#include <hash_table.h>
#include "list.h"
#include "acmod.h"
#include "pocketsphinx_internal.h"
#include "ps_alignment.h"
#include "strfuncs.h"
#include "state_align_search.h"
#include "yasp.h"
#include "cJSON.h"

char *g_modeldir = NULL;

static void redirect_ps_log(err_cb_f cb, struct yasp_logs *logs)
{
	/* disable pocketsphinx logging */
	err_set_logfp(NULL);

	if (!cb || !logs || !logs->lg_error || !logs->lg_info)
		return;

	err_set_callback(cb, logs);
}

static ps_decoder_t *get_ps(cmd_ln_t **config_pp)
{
	cmd_ln_t *config = *config_pp;
	ps_decoder_t *ps = NULL;
	char *hmm, *lm, *dict;

	/* NOTE: the '/' will need to change to support other OSs */
	if (g_modeldir) {
		hmm = string_join(g_modeldir, "/en-us/en-us", NULL);
		lm = string_join(g_modeldir, "/en-us/en-us.lm.bin", NULL);
		dict = string_join(g_modeldir, "/en-us/cmudict-en-us.dict",
				   NULL);
	} else {
		hmm = string_join(MODELDIR, "/en-us/en-us", NULL);
		lm = string_join(MODELDIR, "/en-us/en-us.lm.bin", NULL);
		dict = string_join(MODELDIR, "/en-us/cmudict-en-us.dict",
				   NULL);
	}

	if (!hmm || !lm || !dict) {
		E_ERROR("Failed to allocate ps_decoder_t. No memory\n");
		goto out;
	}

	config = cmd_ln_init(NULL, ps_args(), TRUE,
			"-hmm", hmm,
			"-lm", lm,
			"-dict", dict,
			"-dictcase", "yes",
			"-backtrace", "yes",
			"-dither", "yes",
			"-remove_silence", "no",
			"-cmn", "batch",
			"-beam", "le-20",
			"-pbeam", "le-20",
			"-lw", "2.0",
			NULL);

	if (!config) {
		E_ERROR("Failed to create config object, see log for details\n");
		goto out;
	}

	ps = ps_init(config);
	if (!ps)
		E_ERROR("Failed to create recognizer, see log for details\n");

out:
	if (hmm)
		ckd_free(hmm);
	if (lm)
		ckd_free(lm);
	if (dict)
		ckd_free(dict);

	return ps;
}

static int parse_segments(ps_decoder_t *ps, struct list_head *seg_list)
{
	ps_seg_t *seg;
	struct yasp_word *word = NULL;

	for (seg = ps_seg_iter(ps); seg; seg = ps_seg_next(seg)) {
		const char *segment;
		int sf, ef;
		int32 post, lscr, ascr, lback;

		word = calloc(1, sizeof(*word));
		if (!word)
			goto fail;

		INIT_LIST_HEAD(&word->ph_on_list);

		segment = ps_seg_word(seg);
		ps_seg_frames(seg, &sf, &ef);
		post = ps_seg_prob(seg, &ascr, &lscr, &lback);

		word->ph_word = calloc(1, strlen(segment) + 1);
		if (!word->ph_word) {
			free(word);
			goto fail;
		}

		strncpy(word->ph_word, segment, strlen(segment));
		word->ph_start = sf;
		word->ph_end = ef;
		word->ph_duration = ef - sf;
		word->ph_prob = logmath_exp(ps_get_logmath(ps), post);
		word->ph_lscr = lscr;
		word->ph_ascr = ascr;
		word->ph_lback = lback;

		list_add_tail(&word->ph_on_list, seg_list);
	}

	return 0;

fail:
	yasp_free_segment_list(seg_list);
	return -1;
}

static int set_search_internal(ps_decoder_t *ps, ps_search_t *search)
{
	ps_search_t *old_search;

	if (!search)
		return -1;

	search->pls = ps->phone_loop;
	old_search = (ps_search_t *) hash_table_replace(ps->searches,
							ps_search_name(search),
							search);
	if (old_search != search)
		ps_search_free(old_search);

	return 0;
}

static int parse_alignment(ps_decoder_t *ps, ps_alignment_t *alignment,
			   struct list_head *phoneme_list)
{
	ps_alignment_iter_t* it;
	struct yasp_word *word = NULL;
	char *ph;

	for (it = ps_alignment_phones(alignment); it;
		it = ps_alignment_iter_next(it)) {
		ps_alignment_entry_t* pe
			= ps_alignment_iter_get(it);
		word = calloc(1, sizeof(*word));
		if (!word)
			goto fail;

		INIT_LIST_HEAD(&word->ph_on_list);

		ph = ps->dict->mdef->ciname[pe->id.pid.cipid];
		word->ph_word = calloc(1, strlen(ph)+1);
		if (!word->ph_word) {
			free(word);
			goto fail;
		}
		strncpy(word->ph_word, ph, strlen(ph));
		word->ph_start = pe->start;
		word->ph_duration = pe->duration;
		word->ph_lscr = pe->score;

		list_add_tail(&word->ph_on_list, phoneme_list);
	}

	return 0;

fail:
	yasp_free_segment_list(phoneme_list);
	return -1;
}

static int set_align(ps_decoder_t *ps, const char *name,
		     const char *text, ps_alignment_t **alignment)
{
	ps_search_t *search;
	char *textbuf = ckd_salloc(text);
	char *ptr, *word, delimfound;
	int n;

	textbuf = string_trim(textbuf, STRING_BOTH);
	*alignment = ps_alignment_init(ps->d2p);
	ps_alignment_add_word(*alignment, dict_wordid(ps->dict, "<s>"), 0);
	for (ptr = textbuf;
		(n = nextword(ptr, " \t\n\r", &word, &delimfound)) >= 0;
		ptr = word + n, *ptr = delimfound) {
		int wid;
		if ((wid = dict_wordid(ps->dict, word)) == BAD_S3WID) {
			E_ERROR("Unknown word %s\n", word);
			ckd_free(textbuf);
			ps_alignment_free(*alignment);
			return -1;
		}
		ps_alignment_add_word(*alignment, wid, 0);
	}
	ps_alignment_add_word(*alignment, dict_wordid(ps->dict, "</s>"), 0);
	ps_alignment_populate(*alignment);
	search = state_align_search_init(name, ps->config, ps->acmod, *alignment);
	ckd_free(textbuf);
	return set_search_internal(ps, search);
}

static void *cache_file(FILE *fh, size_t *size)
{
	size_t nbytes;
	void *buf;

	if (!fh)
		return NULL;

	/* go to the end of the file */
	fseek(fh, 0, SEEK_END);
	/* count the number of bytes in the file */
	nbytes = ftell(fh);
	/* allocate the buffer for the transcript */
	buf = calloc(nbytes+1, 1);
	if (!buf) {
		E_ERROR("out of memory\n");
		return NULL;
	}
	/* seek to the beginning of the file in prep to read */
	fseek(fh, 0, SEEK_SET);
	/* read file */
	if (fread(buf, 1, nbytes, fh) != nbytes) {
		E_ERROR("Failed to fully read in file\n");
		free(buf);
		return NULL;
	}

	if (size)
		*size = nbytes;

	return buf;
}

static int interpret(FILE *fh, struct list_head *word_list,
		     struct list_head *phoneme_list,
		     FILE *transcript_fh)
{
	int rc = 0;
	ps_decoder_t *ps = NULL;
	cmd_ln_t *config = NULL;
	char *text = NULL;
	ps_alignment_t *alignment = NULL;

	ps = get_ps(&config);
	if (!ps)
		goto out;

	if (!transcript_fh)
		goto skip_transcript;

	text = cache_file(transcript_fh, NULL);
	if (!text)
		goto out;

	rc = set_align(ps, "align", text, &alignment);
	if (rc) {
		E_ERROR("set_align failed\n");
		goto out;
	}

	if (ps_set_search(ps, "align")) {
		E_ERROR("ps_set_search() failed\n");
		rc = -1;
		goto out;
	}

skip_transcript:
	ps_decode_raw(ps, fh, -1);

	if ((rc = parse_segments(ps, word_list)))
		goto out;

	if (!phoneme_list)
		goto out;

	rc = parse_alignment(ps, alignment, phoneme_list);

out:
	if (alignment)
		ps_alignment_free(alignment);
	if (ps)
		ps_free(ps);
	if (config)
		cmd_ln_free_r(config);
	if (text)
		free(text);

	return rc;
}

static FILE *write_hypothesis_2_file(struct list_head *words,
				     const char *gen_path)
{
	FILE *fh;
	const char *fname;
	struct yasp_word *word;

	if (gen_path)
		fname = gen_path;
	else
		fname = "generated_hypothesis";

	fh = fopen(fname, "w");

	list_for_each_entry(word, words, ph_on_list) {
		if (!strcmp(word->ph_word, "<s>") ||
		    !strcmp(word->ph_word, "</s>") ||
		    !strcmp(word->ph_word, "<sil>"))
			continue;
		fprintf(fh, "%s ", word->ph_word);
	}

	fclose(fh);

	fh = fopen(fname, "r");

	return fh;
}

static int get_utterance(FILE *fh, FILE *transcript_fh,
			 struct list_head *word_list,
			 struct list_head *phoneme_list,
			 const char *gen_path)
{
	int rc;
	struct list_head local_hypothesis;
	FILE *local_fh = transcript_fh;

	INIT_LIST_HEAD(&local_hypothesis);

	/*
	 * if there is no transcript provided, we'll create our own by
	 * getting a hypothesis, writing it into a file and then using
	 * that to get the phonemes
	 */
	if (!transcript_fh) {
		rc = interpret(fh, &local_hypothesis, NULL, NULL);
		if (rc)
			return rc;
		local_fh
		  = write_hypothesis_2_file(&local_hypothesis, gen_path);
		yasp_free_segment_list(&local_hypothesis);
		if (!local_fh)
			return rc;
	}

	rc = interpret(fh, word_list, phoneme_list, local_fh);

	if (!local_fh)
		fclose(local_fh);

	return rc;
}

void yasp_set_modeldir(const char *modeldir)
{
	if (!modeldir)
		g_modeldir = (char*) modeldir;
}

void yasp_free_segment_list(struct list_head *seg_list)
{
	struct yasp_word *word = NULL;
	struct yasp_word *tmp;

	if (!seg_list)
		return;

	/* free any allocated memory */
	list_for_each_entry_safe(word, tmp, seg_list, ph_on_list) {
		list_del(&word->ph_on_list);
		if (word->ph_word)
			free(word->ph_word);
		free(word);
	}
}

void yasp_print_segment_list(struct list_head *seg_list)
{
	struct yasp_word *word;

	if (!seg_list)
		return;

	E_INFO("XXXXXXXXXXXXXXXXXXXXXX\n");
	E_INFO("%s %s %s %s %s %s %s %s\n",
		"word", "start", "end", "pprob", "ascr", "lscr",
		"lback", "duration");

	list_for_each_entry(word, seg_list, ph_on_list) {
		E_INFO("%s %d %d %f %d %d %d %d\n",
			word->ph_word, word->ph_start, word->ph_end,
			word->ph_prob, word->ph_lscr, word->ph_ascr,
			word->ph_lback, word->ph_duration);
	}
	E_INFO("XXXXXXXXXXXXXXXXXXXXXX\n\n\n");

}

void yasp_pprint_segment_list(struct list_head *seg_list)
{
	struct yasp_word *word;

	if (!seg_list)
		return;

	E_INFO("XXXXXXXXXXXXXXXXXXXXXX\n");
	E_INFO("%-20s %-5s %-5s %-5s %-10s %-10s %-3s\n",
		"word", "start", "end", "pprob", "ascr", "lscr",
		"lback");

	list_for_each_entry(word, seg_list, ph_on_list) {
		E_INFO("%-20s %-5d %-5d %-1.3f %-10d %-10d %-3d\n",
			word->ph_word, word->ph_start, word->ph_end,
			word->ph_prob, word->ph_lscr, word->ph_ascr,
			word->ph_lback);
	}
	E_INFO("XXXXXXXXXXXXXXXXXXXXXX\n\n\n");

}


int set_transcript_word(struct list_head *transcript, char * word)
{
	struct yasp_word *hyp_word = NULL;

	/* end of word */
	hyp_word = calloc(1, sizeof(*hyp_word));
	if (!hyp_word)
		return -ENOMEM;

	hyp_word->ph_word = word;
	list_add(&hyp_word->ph_on_list, transcript);

	return 0;
}

int yasp_parse_transcript(struct list_head *transcript, FILE *fh)
{
	char *word = NULL;
	int ccount = 0;
	int rc = 0;

	/* read the transcript and breakdown into words */
	/*
	 * read a byte at a time, and check to see if the byte is a ' ',
	 * if it is, then it's the end of the word.
	 */
	word = calloc(1, sizeof(*word) * 1024);
	if (!word)
		goto failed;
	while (!feof(fh)) {
		size_t nread;
		char c;
		nread = fread(&c, 1, 1, fh);
		if (nread != 1)
			goto failed;
		if (c != ' ' && ccount < 1023) {
			word[ccount] = c;
			ccount++;
		} else if (ccount > 0 && ccount < 1023) {
			rc = set_transcript_word(transcript, word);
			if (rc)
				goto failed;

			/* allocate new word */
			word = calloc(1, sizeof(*word) * 1024);
			if (!word)
				goto failed;
			ccount = 0;
		} else if (ccount >= 1023) {
			/* word is too large */
			E_ERROR("Word is too large in "
				"transcript\n");
			goto failed;
		}
		/* skip white spaces */
	}
	/* deal with the last word before eof */
	if (ccount > 0 && ccount < 1024) {
		rc = set_transcript_word(transcript, word);
		if (rc)
			goto failed;
	}

	return 0;

failed:
	E_ERROR("Failed due to: %s\n", strerror(errno));
	if (word)
		free(word);
	return -1;
}

/*
 * the assumption here is that word_list and phoneme list are generated
 * via the same transcript (whether user provided or auto-generated), so
 * they should match exactly
 *
 * Phoneme list will have relative start time within the utterance and
 * the correct duration for each phoneme. This function will correct the
 * time by adding the offset
 */
static int consolidate_utterance(struct list_head *word_list,
				 struct list_head *phoneme_list)
{
	struct yasp_word *word;
	struct yasp_word *phoneme;
	int offset = -1;

	if (!word_list || !phoneme_list)
		return -1;

	list_for_each_entry(word, word_list, ph_on_list) {
		if (!strcmp(word->ph_word, "<s>"))
			offset = word->ph_start;
	}

	if (offset == -1)
		return -1;

	list_for_each_entry(phoneme, phoneme_list, ph_on_list)
		phoneme->ph_start += offset;

	return 0;
}

static int
consolidate(const char *audioFile, const char *transcript,
	    struct list_head *word_list,
	    struct list_head *phoneme_list,
	    const char *genpath)
{
	FILE *fh;
	FILE *transcript_fh = NULL;
	int rc;

	if (!word_list || !phoneme_list) {
		E_ERROR("bad parameter\n");
		return -1;
	}

	/* Open audio File */
	fh = fopen(audioFile, "rb");
	if (!fh) {
		E_ERROR("unable to open audio file %s. errno = %s\n",
			audioFile, strerror(errno));
		return -1;
	}

	if (transcript) {
		/* consolidate hypothesis with transcript */
		transcript_fh = fopen(transcript, "r");
		if (!transcript_fh) {
			E_ERROR("unable to open transcript %s. errno = %s\n",
				transcript, strerror(errno));
			return -1;
		}
	}

	/* Get the phonemes */
	rc = get_utterance(fh, transcript_fh, word_list, phoneme_list,
			   genpath);

	if (!rc) {
		rc = consolidate_utterance(word_list, phoneme_list);
		if (rc)
			E_ERROR("Timing incompatibility between word and "
				"phoneme lists. Result maybe unreliable\n");
	}

	/* close files */
	fclose(fh);
	if (transcript)
		fclose(transcript_fh);

	return rc;
}

/*
 * {
 *   "words": [
 *       {
 *           "word": "blah",
 *           "start": 1280,
 *           "duration": 720,
 *           "phonemes": [
 *                {
 *                     "phoneme": "EH",
 *                     "start": 23,
 *                     "duration": 2,
 *                },
 *           ],
 *       },
 *       {
 *           "word": "blah2",
 *           "start": 1280,
 *           "duration": 720,
 *           "phonemes": [
 *                {
 *                     "phoneme": "EH",
 *                     "start": 23,
 *                     "duration": 2,
 *                },
 *           ],
 *       },
 *   ],
 * }
 */
char *yasp_create_json(struct list_head *word_list,
		       struct list_head *phoneme_list)
{
	struct yasp_word *word;
	struct yasp_word *phoneme;
	struct list_head *cur;
	int next_time;
	char *string = NULL;
	cJSON *jroot, *jword, *jwords;
	cJSON *jphoneme, *jphonemes;

	if (!word_list || !phoneme_list) {
		E_ERROR("bad parameter list\n");
		return NULL;
	}

	jroot = cJSON_CreateObject();
	if (!jroot)
		goto end;

	jwords = cJSON_AddArrayToObject(jroot, "words");
	if (!jwords)
		goto end;

	cur = phoneme_list;
	list_for_each_entry(word, word_list, ph_on_list) {
		if (!strcmp(word->ph_word, "<s>") ||
		    !strcmp(word->ph_word, "</s>") ||
		    !strcmp(word->ph_word, "<sil>"))
			continue;

		jword = cJSON_CreateObject();
		if (!cJSON_AddStringToObject(jword, "word", word->ph_word))
			goto end;
		if (!cJSON_AddNumberToObject(jword, "start",
					     word->ph_start))
			goto end;
		if (!cJSON_AddNumberToObject(jword, "duration",
					     word->ph_duration))
			goto end;
		jphonemes = cJSON_AddArrayToObject(jword,
						  "phonemes");
		if (!jphonemes)
			goto end;

		list_for_each_entry(phoneme, cur, ph_on_list) {
			next_time =
			  phoneme->ph_start + phoneme->ph_duration + 1;
			if (!strcmp(phoneme->ph_word, "SIL"))
				goto skip_phoneme;

			jphoneme = cJSON_CreateObject();
			if (!cJSON_AddStringToObject(jphoneme, "phoneme",
						     phoneme->ph_word))
				goto end;
			if (!cJSON_AddNumberToObject(jphoneme, "start",
						     phoneme->ph_start))
				goto end;
			if (!cJSON_AddNumberToObject(jphoneme, "duration",
						     phoneme->ph_duration))
				goto end;
			cJSON_AddItemToArray(jphonemes, jphoneme);

skip_phoneme:
			if (next_time > word->ph_end) {
				/* start the next word */
				cur = &phoneme->ph_on_list;
				break;
			}
		}

		cJSON_AddItemToArray(jwords, jword);
	}

	string = cJSON_Print(jroot);
	if (!string) {
		E_ERROR("Failed to print json file\n");
		goto end;
	}

end:
	cJSON_Delete(jroot);
	return string;
}

int yasp_create_json_file(struct list_head *word_list,
			  struct list_head *phoneme_list,
			  const char *output)
{
	FILE *json_fh;
	char *json_str;
	int rc = 0;

	json_str = yasp_create_json(word_list, phoneme_list);
	if (!json_str) {
		rc = -EINVAL;
		goto end;
	}

	json_fh = fopen(output, "w");
	if (!json_fh) {
		E_ERROR("Failed to open output: %s\n", output);
		rc = -errno;
		goto end;
	}

	fprintf(json_fh, "%s", json_str);
	fclose(json_fh);

end:
	if (json_str)
		free(json_str);
	return rc;
}

int yasp_interpret_hypothesis(const char *faudio, const char *ftranscript,
			      const char *genpath,
			      struct list_head *word_list)
{
	struct list_head phoneme_list;
	int rc;

	INIT_LIST_HEAD(&phoneme_list);

	if (!word_list) {
		E_ERROR("bad parameter\n");
		return -EINVAL;
	}

	/*
	 * Parse audio file
	 */
	rc = consolidate(faudio, ftranscript, word_list,
			 &phoneme_list, genpath);
	if (rc)
		E_ERROR("Failed to parse speech clip %s\n",
			faudio);

	yasp_free_segment_list(&phoneme_list);

	return rc;
}

int yasp_interpret_phonemes(const char *faudio, const char *ftranscript,
			    const char *genpath,
			    struct list_head *phoneme_list)
{
	struct list_head word_list;
	int rc;

	INIT_LIST_HEAD(&word_list);

	if (!phoneme_list) {
		E_ERROR("bad parameter\n");
		return -EINVAL;
	}

	/*
	 * Parse audio file
	 */
	rc = consolidate(faudio, ftranscript, &word_list,
			 phoneme_list, genpath);
	if (rc)
		E_ERROR("Failed to parse speech clip %s\n",
			faudio);

	yasp_free_segment_list(&word_list);

	return rc;
}

static int
yasp_interpret_helper(const char *audioFile, const char *transcript,
		      const char *output, const char *genpath,
		      char **json, bool write)
{
	int rc;
	struct list_head word_list;
	struct list_head phoneme_list;

	INIT_LIST_HEAD(&word_list);
	INIT_LIST_HEAD(&phoneme_list);

	/*
	 * Parse audio file
	 */
	rc = consolidate(audioFile, transcript, &word_list,
			 &phoneme_list, genpath);
	if (rc) {
		E_ERROR("Failed to parse speech clip %s\n",
			audioFile);
		goto out;
	}

	if (write) {
		if (!output)
			goto out;

		rc = yasp_create_json_file(&word_list, &phoneme_list,
					   output);
		if (rc) {
			E_ERROR("Failed to create json file: %s\n",
				output);
			goto out;
		}
	} else {
		if (!json) {
			rc = -EINVAL;
			goto out;
		}
		*json = yasp_create_json(&word_list, &phoneme_list);
	}

out:

	yasp_print_segment_list(&word_list);
	yasp_print_segment_list(&phoneme_list);

	yasp_free_segment_list(&word_list);
	yasp_free_segment_list(&phoneme_list);

	return rc;
}

void yasp_free_json_str(char *json)
{
	if (json)
		free(json);
}

char *
yasp_interpret_get_str(const char *audioFile,
		       const char *transcript,
		       const char *genpath)
{
	int rc;
	char *json = NULL;

	rc = yasp_interpret_helper(audioFile, transcript, NULL,
				   genpath, &json, false);

	if (rc)
		return NULL;

	return json;
}

int
yasp_interpret(const char *audioFile, const char *transcript,
	       const char *output, const char *genpath)
{
	return yasp_interpret_helper(audioFile, transcript, output,
				     genpath, NULL, true);
}

int yasp_interpret_breadown(const char *audioFile, const char *transcript,
			    const char *output, const char *genpath,
			    struct list_head *word_list,
			    struct list_head *phoneme_list)
{
	int rc;

	if (!word_list || !phoneme_list) {
		E_ERROR("bad arguments\n");
		return -1;
	}

	/*
	 * Parse audio file
	 */
	rc = consolidate(audioFile, transcript, word_list,
			 phoneme_list, genpath);
	if (rc)
		E_ERROR("Failed to parse speech clip %s\n",
			audioFile);

	return rc;
}

void yasp_log(void *user_data, err_lvl_t el, const char *fmt, ...)
{
	struct yasp_logs *logs = user_data;
	va_list ap;

	if (!logs || !logs->lg_error || !logs->lg_info)
		return;

	va_start(ap, fmt);
	if (el == ERR_INFO || el == ERR_DEBUG ||
	    el == ERR_INFOCONT)
		vfprintf(logs->lg_info, fmt, ap);
	else
		vfprintf(logs->lg_error, fmt, ap);
	va_end(ap);
}

void yasp_setup_logging(struct yasp_logs *logs, err_cb_f cb,
			const char *logfile)
{
	char *err_logfile;
	const char *err_ext = "_err";

	if (!cb)
		cb = yasp_log;

	if (!logfile || !logs)
		return;

	err_logfile = calloc(1, strlen(logfile) + strlen(err_ext) + 1);
	if (!err_logfile) {
		fprintf(stderr, "no memory\n");
		return;
	}

	strcpy(err_logfile, logfile);
	strcat(err_logfile, err_ext);
	logs->lg_error = fopen(err_logfile, "a");
	logs->lg_info = fopen(logfile, "a");

	free(err_logfile);

	if (!logs->lg_error || !logs->lg_info)
		return;

	redirect_ps_log(cb, logs);
}

void yasp_finish_logging(struct yasp_logs *logs)
{
	if (!logs)
		return;

	if (logs->lg_error)
		fclose(logs->lg_error);

	if (logs->lg_info)
		fclose(logs->lg_info);
}

int
main(int argc, char *argv[])
{
	int rc;
	int opt;
	const char *audioFile = NULL;
	const char *transcript = NULL;
	const char *genpath = NULL;
	const char *output = NULL;
	const char *logfile = "default_log";
	struct list_head word_list;
	struct yasp_logs logs;

	INIT_LIST_HEAD(&word_list);

	const char *const short_options = "a:t:o:g:l:m:h";
	static const struct option long_options[] = {
		{ .name = "audio", .has_arg = required_argument, .val = 'a' },
		{ .name = "transcript", .has_arg = required_argument, .val = 't' },
		{ .name = "output", .has_arg = required_argument, .val = 'o' },
		{ .name = "genpath", .has_arg = required_argument, .val = 'g' },
		{ .name = "logfile", .has_arg = required_argument, .val = 'l' },
		{ .name = "modeldir", .has_arg = required_argument, .val = 'm' },
		{ .name = "help", .has_arg = no_argument, .val = 'h' },
		{ .name = NULL },
	};

	while ((opt = getopt_long(argc, argv, short_options,
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'a':
			audioFile = optarg;
			break;
		case 't':
			transcript = optarg;
			break;
		case 'o':
			output = optarg;
			break;
		case 'g':
			genpath = optarg;
			break;
		case 'm':
			yasp_set_modeldir(optarg);
			break;
		case 'h':
			printf("Usage: \n"
			       "run -a </path/to/audio/file> "
			       "-t [</path/to/audio/transcript>] "
                   "-g [</path/to/genfile>] "
                   "-m [</path/to/modeldir>]\n");
			return -1;
		default:
			E_ERROR("Unknown command line option\n");
			return -1;
		}
	}

	if (!audioFile) {
		E_ERROR("No audio file provided. Please provide one\n");
		return -1;
	}

	yasp_setup_logging(&logs, NULL, logfile);

	rc = yasp_interpret(audioFile, transcript, output, genpath);
	if (rc)
		E_ERROR("Failed to interpret audio file %s\n",
			audioFile);

	//rc = yasp_interpret_hypothesis(audioFile, transcript, genpath,
	//			       &word_list);
	//yasp_free_segment_list(&word_list);

	yasp_finish_logging(&logs);

	return rc;
}

