#pragma varargck argpos sendcrnl 2
#pragma varargck argpos senderr 2
#pragma varargck argpos sendok 1
#pragma varargck argpos csyslog 1

enum {
	timeout = 20000,	/* amount of time in ms to wait for response */
};

enum
{
	Stacksz	= 64*1024,
};

enum
{
	Maxconf	= 16,
	Nline = 8192,
};

#define HAport	"1007"
#define	pr	print
#define stateenv "#ec/hastate"
#define statefile "/n/kfs/conf/ha/state"
#define	HAactive	"active"
#define	HAinactive	"inactive"
#define peereafile "/n/kfs/conf/ha/peerea"

typedef	struct	Conn	Conn;
typedef struct 	Cmd 	Cmd;
typedef struct	User	User;
typedef struct	Smap	Smap;

struct	Conn
{
	int	fd;
	char	dir[40];
};

struct Cmd
{
	char	*name;
	int 	needauth;
	int	hacmd;
	char	*(*f)(int, char **);
};

struct	User
{
	char	*raddr;
	char	*laddr;
	int	fd;
	int	loggedin;
	int	rmtactive;
	char	*user;
	char	*chal;
	Biobuf	in;
	Biobuf	out;
};

struct	Smap
{
	int	ostate;
	int	state;
	int	(*fn)(void);
};

extern Cmd cmdtab[];		/* Must be defined by code using util.c */
extern char *myrole;
extern int debug;
extern char *rootdir;
extern User *u;
