
char	*getstate(void);			/* Defined by user of util.c */

void	exitsall(char *);
void	killapp(int);
void	error(char *);
char	*basename(char *);
int	srvfd(char *, int, int);
void	setprompt(void);
void	csyslog(char *, ...);
int	getcrnlto(User *, char *, int);
int	getcrnl(User *, char *, int);
void	sendcrnl(Biobuf *, char *, ...);
int	sendok(char *, ...);
int	senderr(Biobuf *, char *, ...);
int	checkrole(char **);
void	cmd(void);
char	*cmd_exec(int, char *);
char	*versioncmd(int, char **);
char	*helpcmd(int, char**);
char	*echocmd(int, char **);
char	*statuscmd(int , char **);
char	*eladdrcmd(int , char **);

int	haopenconfig(char *);
int	hawriteconfig(char *, ulong, char *, int, int);

int	receiveaddrs(User *u, int active);
void	sendaddrs(Biobuf *out);
char	*killws(char *);
int	rmall(char *);
