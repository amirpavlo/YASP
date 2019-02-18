%module yasp

%{
struct yasp_logs {
	FILE *lg_error;
	FILE *lg_info;
};

typedef enum err_e {
    ERR_DEBUG,
    ERR_INFO,
    ERR_INFOCONT,
    ERR_WARN,
    ERR_ERROR,
    ERR_FATAL,
    ERR_MAX
} err_lvl_t;

typedef void (*err_cb_f)(void * user_data, err_lvl_t, const char *, ...);

extern int yasp_interpret(const char *audioFile, const char *transcript,
                          const char *output, const char *genpath);
extern char *yasp_interpret_get_str(const char *audioFile,
                                    const char *transcript,
                                    const char *genpath);
extern void yasp_setup_logging(struct yasp_logs *logs, err_cb_f cb,
                               const char *logfile);
extern void yasp_finish_logging(struct yasp_logs *logs);
extern void yasp_set_modeldir(const char *modeldir);
%}

struct yasp_logs {
	FILE *lg_error;
	FILE *lg_info;
};

typedef enum err_e {
    ERR_DEBUG,
    ERR_INFO,
    ERR_INFOCONT,
    ERR_WARN,
    ERR_ERROR,
    ERR_FATAL,
    ERR_MAX
} err_lvl_t;

typedef void (*err_cb_f)(void * user_data, err_lvl_t, const char *, ...);

extern int yasp_interpret(const char *audioFile, const char *transcript,
                          const char *output, const char *genpath);
extern char *yasp_interpret_get_str(const char *audioFile,
                                    const char *transcript,
                                    const char *genpath);
extern void yasp_setup_logging(struct yasp_logs *logs, err_cb_f cb,
                               const char *logfile);
extern void yasp_finish_logging(struct yasp_logs *logs);
extern void yasp_set_modeldir(const char *modeldir);

