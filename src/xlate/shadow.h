typedef struct
{
	ulong flags;
	char *ipaddr;
	uchar certhash[SHA1dlen];
} Node;

enum
{
	Nencrypt = 1,
};

typedef struct
{
	char *name;
	Node *n[2];
} Remote;

Remote *remoteget(char *name);
Remote *remotegetip(char *ipaddr);
void remotefree(Remote *r);
Node *remotenode(Remote *r, char *ipaddr);

Node *nodeget(char *ipaddr);
void nodefree(Node *r);

int shadowclient(char *remote, char *cert, int debug);
int shadowserver(char *ldial, char *cert);
int shadowserverdebug;

enum
{
	Ciphersmasknull		= 0x40, // TLS_RSA_WITH_NULL_SHA
	Ciphersmaskencrypt	= 0x08, // TLS_RSA_WITH_AES_128_CBC_SHA
};

extern int shadowport;

char *leakcheck(int pid);

void tlsconnfree(TLSconn *tlsc);

typedef struct
{
	Cmdtab	*tab;
	int	 ntab;
	Cmdbuf	*cb;
	Cmdtab	*cmd;
	int	 dbfd;
	int	 tsecs;	// if 0 use the default value, else override
} Cmdbufidx;

void cbiinit(Cmdbufidx *cbi);
int Brdcmd(Biobufhdr *br, Cmdbufidx *cbi);
int Bnextcmd(Biobufhdr *br, Biobufhdr *bw, Cmdbufidx *cbi, int cmdindex);

// shadow handshake commands
enum
{
	SHSsend,
	SHSver,
	SHSsnap,
	SHSsize,
	SHScopy,
	SHSdata,
	SHSend,
};

int reporter(int repfd, char *fmt, ...);
int reperrstr(int repfd, char *fmt, ...);

#pragma varargck argpos reporter 2
