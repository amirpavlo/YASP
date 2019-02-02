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

static void setup_logging(struct yasp_logs *logs, const char *logfile)
{
	char *err_logfile;
	const char *err_ext = "_err";

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

	if (!logs->lg_error || !logs->lg_info)
		return;

	yasp_redirect_ps_log(yasp_log, logs);
}

static void finish_logging(struct yasp_logs *logs)
{
	if (!logs)
		return;

	if (logs->lg_error)
		fclose(logs->lg_error);

	if (logs->lg_info)
		fclose(logs->lg_info);
}

static ps_decoder_t *get_ps(bool word, cmd_ln_t **config_pp)
{
	cmd_ln_t *config = *config_pp;
	ps_decoder_t *ps;

	if (word) {
		config = cmd_ln_init(NULL, ps_args(), TRUE,
				"-hmm", MODELDIR "/en-us/en-us",
				"-lm", MODELDIR "/en-us/en-us.lm.bin",
				"-dict", MODELDIR "/en-us/cmudict-en-us.dict",
				"-dictcase", "yes",
				NULL);
	} else {
		config = cmd_ln_init(NULL, ps_args(), TRUE,
				"-hmm", MODELDIR "/en-us/en-us",
				"-lm", MODELDIR "/en-us/en-us.lm.bin",
				"-allphone", MODELDIR "/en-us/en-us-phone.lm.bin",
				"-backtrace", "yes",
				"-beam", "le-20",
				"-pbeam", "le-20",
				"-lw", "2.0",
				"-dict", MODELDIR "/en-us/cmudict-en-us.dict",
				"-dictcase", "yes",
				NULL);
	}

	if (!config) {
		E_ERROR("Failed to create config object, see log for details\n");
		return NULL;
	}

	ps = ps_init(config);
	if (!ps) {
		E_ERROR("Failed to create recognizer, see log for details\n");
		return NULL;
	}

	return ps;
}

static int align_hypothesis_transcript(ps_decoder_t *ps, FILE *transcript_fh)
{
	int nchars;
	int rc = 0;
	char *text = NULL;

	if (!ps || !transcript_fh)
		return 0;

	/* go to the beginning of the file */
	fseek(transcript_fh, 0, SEEK_END);
	/* count the number of characters in the file */
	nchars = ftell(transcript_fh);
	/* allocate the buffer for the transcript */
	text = calloc(nchars + 1, 1);
	/* seek to the beginning of the file in prep to read */
	fseek(transcript_fh, 0, SEEK_SET);
	/* read file */
	if (fread(text, 1, nchars, transcript_fh) != nchars) {
		E_ERROR("Failed to fully read transcript file\n");
		rc = -1;
		goto failed;
	}

	/* Always use the same name so that we don't leak memory (hopefully). */
	if (ps_set_align(ps, "align", text)) {
		E_ERROR("ps_set_align() failed\n");
		rc= -1;
		goto failed;
	}

	if (ps_set_search(ps, "align")) {
		E_ERROR("ps_set_search() failed\n");
		rc = -1;
	}

failed:
	if (text)
		free(text);

	return rc;
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
/*
		E_ERROR("%s %d %d %d\n",
			ps->dict->mdef->ciname[pe->id.pid.cipid],
			pe->start, pe->duration, pe->score);
*/
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

static int parse_speech(FILE *fh, FILE *transcript_fh,
			struct list_head *word_list,
			struct list_head *phoneme_list)
{
	int16 *buf;
	int16 const *bptr;
	int rc = 0;
	int nfr;
	ps_decoder_t *ps;
	cmd_ln_t *config = NULL;
	struct list_head test;
	size_t nsamp;
	char *text = NULL;
	ps_alignment_t *alignment = NULL;

	INIT_LIST_HEAD(&test);

	text = cache_file(transcript_fh, NULL);
	if (!text) {
		rc = -1;
		goto out;
	}

	ps = get_ps(false, &config);
	if (!ps) {
		E_ERROR("FAILED to get decoder\n");
		rc = -1;
		goto out;
	}

	/*
	 * start alignment. In order to get the phoneme alignment I had to
	 * go past the pocketsphinx provided API.
	 */
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

	/* start recognition */
	rc = acmod_start_utt(ps->acmod);
	if (rc) {
		E_ERROR("FAILED test_align start_utt\n");
		goto out;
	}

	ps_search_start(ps->search);

	buf = cache_file(fh, &nsamp);
	if (!buf) {
		rc = -1;
		goto out;
	}
	/*
	 * nsamp comes in as the number of bytes. But we pass in int16
	 * which is 2 bytes, so we need to adjust the initial size of the
	 * buffer to account for that
	 */
	nsamp /= 2;
	bptr = buf;
	/*
	 * TODO: this is not great as we need to read the entire audio
	 * file, which is not optimal if the file is large
	 */
	while ((nfr = acmod_process_raw(ps->acmod, &bptr, &nsamp,
					true)) > 0 || nsamp > 0) {
		while(ps->acmod->n_feat_frame > 0) {
			ps_search_step(ps->search,
					ps->acmod->output_frame);
			acmod_advance(ps->acmod);
		}
	}
	rc = ps_search_finish(ps->search);
	if (rc) {
		E_ERROR("phoneme search failed\n");
		rc = -1;
		goto out;
	}

	rc = parse_segments(ps, word_list);
	if (rc) {
		E_ERROR("failed to parse word_list\n");
		goto out;
	}

	rc = parse_alignment(ps, alignment, phoneme_list);
	if (rc)
		E_ERROR("failed to parse alignment into phoneme"
			" list\n");

out:
	if (alignment)
		ps_alignment_free(alignment);
	if (buf)
		free(buf);
	if (ps)
		ps_free(ps);
	if (config)
		cmd_ln_free_r(config);
	if (text)
		free(text);

	return 0;
}

static int interpret(FILE *fh, struct list_head *list,
		     FILE *transcript_fh, bool word)
{
	int16 buf[512];
	int rc = 0;
	ps_decoder_t *ps = NULL;
	cmd_ln_t *config = NULL;

	ps = get_ps(word, &config);
	if (!ps)
		goto out;

	/* align words */
	rc = align_hypothesis_transcript(ps, transcript_fh);
	if (rc)
		goto out;

	rc = ps_start_utt(ps);

	while (!feof(fh)) {
		size_t nsamp;
		nsamp = fread(buf, 2, 512, fh);
		rc = ps_process_raw(ps, buf, nsamp, FALSE, FALSE);
	}

	rc = ps_end_utt(ps);

	if (parse_segments(ps, list))
		rc = -1;

out:
	if (ps)
		ps_free(ps);
	if (config)
		cmd_ln_free_r(config);

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

	list_for_each_entry(word, words, ph_on_list)
		fprintf(fh, "%s ", word->ph_word);

	fclose(fh);

	fh = fopen(fname, "r");

	return fh;
}

static int get_phonemes(FILE *fh, FILE *transcript_fh,
			struct list_head *word_list,
			struct list_head *phoneme_list,
			const char *gen_path)
{
	int rc;
	struct list_head local_hypothesis;
	FILE *local_transcript = transcript_fh;

	INIT_LIST_HEAD(&local_hypothesis);

	/*
	 * if there is no transcript provided, we'll create our own by
	 * getting a hypothesis, writing it into a file and then using
	 * that to get the phonemes
	 */
	if (!transcript_fh) {
		rc = interpret(fh, &local_hypothesis, NULL, true);
		if (rc)
			return rc;
		local_transcript
		  = write_hypothesis_2_file(&local_hypothesis, gen_path);
		yasp_free_segment_list(&local_hypothesis);
		if (!local_transcript)
			return rc;
	}

	rc = parse_speech(fh, local_transcript, word_list,
			  phoneme_list);

	if (!transcript_fh)
		fclose(local_transcript);

	return rc;
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

int yasp_interpret_hypothesis(const char *faudio, const char *ftranscript,
			      const char *logfile,
			      struct list_head *word_list)
{
	FILE *fh = NULL;
	FILE *transcript_fh = NULL;
	struct yasp_logs logs;
	int rc;

	setup_logging(&logs, logfile);

	if (!word_list) {
		E_ERROR("bad parameter\n");
		return -EINVAL;
	}

	fh = fopen(faudio, "rb");
	if (!fh) {
		E_ERROR("Failed to open file %s. errno %s\n",
			faudio, strerror(errno));
		return -1;
	}

	if (!ftranscript)
		goto out;

	transcript_fh = fopen(ftranscript, "r");
	if (!fh) {
		E_ERROR("Failed to open transcript %s. errno %s\n",
			ftranscript, strerror(errno));
		return -1;
	}

out:
	rc = interpret(fh, word_list, transcript_fh, true);

	yasp_print_segment_list(word_list);

	finish_logging(&logs);

	return rc;
}

int yasp_interpret_phonemes(const char *faudio, const char *ftranscript,
			    const char *logfile,
			    struct list_head *phoneme_list)
{
	FILE *fh = NULL;
	FILE *transcript_fh = NULL;
	struct list_head word_list;
	struct yasp_logs logs;
	int rc;

	INIT_LIST_HEAD(&word_list);

	if (!phoneme_list) {
		E_ERROR("bad parameter\n");
		return -EINVAL;
	}

	setup_logging(&logs, logfile);

	fh = fopen(faudio, "rb");
	if (!fh) {
		E_ERROR("Failed to open file %s. errno %s\n",
			faudio, strerror(errno));
		return -1;
	}

	if (!ftranscript)
		goto out;

	transcript_fh = fopen(ftranscript, "r");
	if (!fh) {
		E_ERROR("Failed to open transcript %s. errno %s\n",
			ftranscript, strerror(errno));
		return -1;
	}

out:
	rc = get_phonemes(fh, transcript_fh, &word_list, phoneme_list,
			  NULL);

	yasp_free_segment_list(&word_list);

	finish_logging(&logs);

	return rc;
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

static int
consolidate(const char *audioFile, const char *transcript,
	    struct list_head *word_list,
	    struct list_head *phoneme_list,
	    const char *path)
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
	rc = get_phonemes(fh, transcript_fh, word_list, phoneme_list,
			  path);

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
int yasp_create_json(struct list_head *word_list,
		     struct list_head *phoneme_list,
		     const char *output)
{
	struct yasp_word *word;
	struct yasp_word *phoneme;
	struct list_head *cur;
	FILE *json_fh;
	int next_time;
	char *string;
	cJSON *jroot, *jword, *jwords;
	cJSON *jphoneme, *jphonemes;

	if (!word_list || !phoneme_list) {
		E_ERROR("bad parameter list\n");
		return -1;
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

	json_fh = fopen(output, "w");
	if (!json_fh) {
		E_ERROR("Failed to open output: %s\n", output);
		goto end;
	}

	fprintf(json_fh, "%s", string);
	fclose(json_fh);

end:
	cJSON_Delete(jroot);
	return 0;
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

void yasp_redirect_ps_log(err_cb_f cb, struct yasp_logs *logs)
{
	/* disable pocketsphinx logging */
	err_set_logfp(NULL);

	if (!cb || !logs || !logs->lg_error || !logs->lg_info)
		return;

	err_set_callback(cb, logs);
}

int yasp_interpret(const char *audioFile, const char *transcript,
		   const char *output, const char *genpath,
		   const char *logfile)
{
	int rc;
	struct yasp_logs logs;
	struct list_head word_list;
	struct list_head phoneme_list;

	INIT_LIST_HEAD(&word_list);
	INIT_LIST_HEAD(&phoneme_list);

	setup_logging(&logs, logfile);

	/*
	 * Parse a list of the phonemes for later consolidation
	 */
	rc = consolidate(audioFile, transcript, &word_list,
			 &phoneme_list, genpath);
	if (rc) {
		E_ERROR("Failed to parse speech clip %s\n",
			audioFile);
		goto out;
	}

	if (!output)
		goto out;

	rc = yasp_create_json(&word_list, &phoneme_list, output);
	if (rc) {
		E_ERROR("Failed to create json file: %s\n",
			output);
		goto out;
	}

out:

	yasp_print_segment_list(&word_list);
	yasp_print_segment_list(&phoneme_list);

	yasp_free_segment_list(&word_list);
	yasp_free_segment_list(&phoneme_list);

	finish_logging(&logs);

	return rc;
}

int yasp_interpret_breadown(const char *audioFile, const char *transcript,
			    const char *output, const char *genpath,
			    const char *logfile,
			    struct list_head *word_list,
			    struct list_head *phoneme_list)
{
	int rc;
	struct yasp_logs logs;

	if (!word_list || !phoneme_list) {
		E_ERROR("bad arguments\n");
		return -1;
	}

	setup_logging(&logs, logfile);

	/*
	 * Parse a list of the phonemes for later consolidation
	 */
	rc = consolidate(audioFile, transcript, word_list,
			 phoneme_list, genpath);
	if (rc)
		E_ERROR("Failed to parse speech clip %s\n",
			audioFile);

	finish_logging(&logs);

	return rc;
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

	INIT_LIST_HEAD(&word_list);

	const char *const short_options = "a:t:h:o:g:l:";
	static const struct option long_options[] = {
		{ .name = "audio", .has_arg = required_argument, .val = 'a' },
		{ .name = "transcript", .has_arg = required_argument, .val = 't' },
		{ .name = "output", .has_arg = required_argument, .val = 'o' },
		{ .name = "genpath", .has_arg = required_argument, .val = 'g' },
		{ .name = "logfile", .has_arg = required_argument, .val = 'l' },
		{ .name = "help", .has_arg = no_argument, .val = 'h' },
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
		case 'h':
			printf("Usage: \n"
			       "sparser </path/to/audio/file> "
			       "</path/to/audio/transcript>\n");
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

	rc = yasp_interpret(audioFile, transcript, output, genpath,
			    logfile);
	if (rc)
		E_ERROR("Failed to interpret audio file %s\n",
			audioFile);

	rc = yasp_interpret_hypothesis(audioFile, transcript, logfile,
				       &word_list);
	yasp_free_segment_list(&word_list);

	return rc;
}

