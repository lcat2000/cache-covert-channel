// Two-process CPU-cache covert channel handshake: A says "早安", B replies "您好".
//
// A driver process forks a fresh pair of children each attempt:
//   Agent A -> core 0,  Agent B -> core 1.
// A and B have separate address spaces and share ONLY one mmap'd page; within it
// two cache lines are one-way channels (A->B, B->A). No pipe/socket/signal/shared
// variable carries the greeting -- only the CPU cache STATE of a line does.
//
// Per slot: the RECEIVER primes the probe line into its L1; the SENDER leaves it
// (bit 1 => still in L1 => fast read) or hammers clflush (bit 0 => globally
// evicted => slow read). Each bit is sent R times, majority-voted. A short frame
// (preamble/SFD/len/payload/XOR) is validated; if a scheduling hiccup corrupts an
// attempt, the driver re-forks and retries -- standard covert-channel practice.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <x86intrin.h>

#define SLOT     30000ULL
#define THRESH   187ULL
#define R        3
#define F_LO     20
#define F_HI     65
#define SAMPLE   85
#define GUARD    16
#define PAGE     4096
#define MAXPAY   6
#define FBYTES   (2 + 1 + 1 + MAXPAY + 1)
#define FBITS    (FBYTES * 8)
#define FSLOTS   (FBITS * R)
#define MAX_ATTEMPTS 40

static inline uint64_t now(void){ unsigned a; return __rdtscp(&a); }
static inline void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s); sched_setaffinity(0,sizeof(s),&s); }
static inline uint64_t tacc(volatile char *p){
    _mm_mfence(); _mm_lfence(); uint64_t a=now(); _mm_lfence();
    (void)*p; _mm_lfence(); return now()-a;
}

struct shm {
    volatile int b_ready, go, attempt;
    volatile uint64_t t_base;
    volatile int a_n, b_n, b_matched;
    unsigned char a_pay[MAXPAY], b_pay[MAXPAY];
    char pad0[PAGE];
    char line_AB[PAGE];
    char line_BA[PAGE];
};

static void build(const unsigned char *m,int len,unsigned char*f){
    memset(f,0,FBYTES); f[0]=0xAA;f[1]=0xAA;f[2]=0x7E;f[3]=(unsigned char)len;
    unsigned char cs=(unsigned char)len; for(int i=0;i<len;i++){f[4+i]=m[i];cs^=m[i];} f[4+MAXPAY]=cs;
}
static int parse(const unsigned char*f,unsigned char*o){
    if(f[0]!=0xAA||f[1]!=0xAA||f[2]!=0x7E)return -1;
    int len=f[3]; if(len<0||len>MAXPAY)return -1;
    unsigned char cs=(unsigned char)len; for(int i=0;i<len;i++){o[i]=f[4+i];cs^=f[4+i];}
    return cs==f[4+MAXPAY]?len:-2;
}
static inline void tx_slot(volatile char*L,int bit,uint64_t s){
    uint64_t e=s+SLOT,fs=s+SLOT*F_LO/100,fe=s+SLOT*F_HI/100;
    while(now()<s)_mm_pause();
    if(bit){while(now()<e)_mm_pause();}
    else{while(now()<fs)_mm_pause();while(now()<fe)_mm_clflush((void*)L);while(now()<e)_mm_pause();}
}
static inline int rx_slot(volatile char*L,uint64_t s){
    while(now()<s)_mm_pause();
    (void)*L;
    uint64_t smp=s+SLOT*SAMPLE/100;
    while(now()<smp)_mm_pause();
    return tacc(L)<THRESH;
}
static void tx(volatile char*L,const unsigned char*f,uint64_t base,long s0){
    for(int k=0;k<FBITS;k++){int bit=(f[k/8]>>(7-(k%8)))&1;
        for(int r=0;r<R;r++)tx_slot(L,bit,base+(uint64_t)(s0+(long)k*R+r)*SLOT);}
}
static void rx(volatile char*L,unsigned char*f,uint64_t base,long s0){
    memset(f,0,FBYTES);
    for(int k=0;k<FBITS;k++){int v=0;
        for(int r=0;r<R;r++)v+=rx_slot(L,base+(uint64_t)(s0+(long)k*R+r)*SLOT);
        f[k/8]=(f[k/8]<<1)|(v*2>R);}
}

static const unsigned char HELLO[]={0xE6,0x97,0xA9,0xE5,0xAE,0x89}; // 早安
static const unsigned char REPLY[]={0xE6,0x82,0xA8,0xE5,0xA5,0xBD}; // 您好

static void agentB(struct shm*sh){
    pin(1);
    volatile char *AB=&sh->line_AB[0],*BA=&sh->line_BA[0];
    unsigned char fout[FBYTES]; build(REPLY,sizeof(REPLY),fout);
    for(int w=0;w<3000;w++){(void)*AB;(void)*BA;}
    _mm_clflush((void*)AB);_mm_clflush((void*)BA);_mm_mfence();
    sh->b_ready=1; while(!sh->go)_mm_pause();
    uint64_t base=sh->t_base;
    unsigned char fin[FBYTES],pay[MAXPAY];
    rx(AB,fin,base,0);
    int n=parse(fin,pay);
    int matched=(n==(int)sizeof(HELLO)&&memcmp(pay,HELLO,n)==0);
    if(matched) tx(BA,fout,base,FSLOTS+GUARD);
    sh->b_n=n; sh->b_matched=matched; memcpy((void*)sh->b_pay,pay,MAXPAY);
    _exit(0);
}
static void agentA(struct shm*sh){
    pin(0);
    volatile char *AB=&sh->line_AB[0],*BA=&sh->line_BA[0];
    unsigned char fout[FBYTES]; build(HELLO,sizeof(HELLO),fout);
    for(int w=0;w<3000;w++){(void)*AB;(void)*BA;}
    _mm_clflush((void*)AB);_mm_clflush((void*)BA);_mm_mfence();
    while(!sh->b_ready)_mm_pause();
    sh->t_base=now()+SLOT; sh->go=1;
    uint64_t base=sh->t_base;
    tx(AB,fout,base,0);
    unsigned char fin[FBYTES],pay[MAXPAY];
    rx(BA,fin,base,FSLOTS+GUARD);
    int n=parse(fin,pay);
    sh->a_n=n; memcpy((void*)sh->a_pay,pay,MAXPAY);
    _exit(0);
}

int main(void){
    struct shm*sh=mmap(NULL,sizeof(*sh),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(sh==MAP_FAILED){perror("mmap");return 1;}

    int attempt=0, ok=0;
    for(;attempt<MAX_ATTEMPTS;attempt++){
        // reset per-attempt state
        sh->b_ready=0; sh->go=0; sh->t_base=0;
        sh->a_n=-9; sh->b_n=-9; sh->b_matched=0;
        memset((void*)sh->a_pay,0,MAXPAY); memset((void*)sh->b_pay,0,MAXPAY);
        memset(sh->line_AB,0,PAGE); memset(sh->line_BA,0,PAGE);

        pid_t a=fork(); if(a==0) agentA(sh);
        pid_t b=fork(); if(b==0) agentB(sh);
        waitpid(a,NULL,0); waitpid(b,NULL,0);

        ok = (sh->b_matched &&
              sh->a_n==(int)sizeof(REPLY) &&
              memcmp((void*)sh->a_pay,REPLY,sizeof(REPLY))==0);
        if(ok) break;
    }

    printf("---- transcript (carried over CPU cache state only) ----\n");
    printf("Agent A  ──「早安」─▶   (E6 97 A9 E5 AE 89) on cache line A→B\n");
    printf("Agent B  heard      : ");
    for(int i=0;i<(sh->b_n<0?0:sh->b_n);i++)printf("%02X ",sh->b_pay[i]);
    printf("→ \"%.*s\"  %s\n",sh->b_n<0?0:sh->b_n,(char*)sh->b_pay,
           sh->b_matched?"= 早安  ⇒ 回覆「您好」":"(未對上)");
    printf("Agent A  heard reply: ");
    for(int i=0;i<(sh->a_n<0?0:sh->a_n);i++)printf("%02X ",sh->a_pay[i]);
    printf("→ \"%.*s\"\n",sh->a_n<0?0:sh->a_n,(char*)sh->a_pay);
    printf("\n================= RESULT =================\n");
    printf(" A 說「早安」→ B 收到 → B 回「您好」→ A 收到\n");
    printf(" Handshake : %s   (attempt %d/%d)\n",
           ok?"SUCCESS ✅":"FAILED", attempt+(ok?1:0), MAX_ATTEMPTS);
    printf(" Transport : Flush+Reload cache covert channel, cross-core\n");
    printf("             %llu cyc/slot · %d× repetition · frame %d B\n",
           (unsigned long long)SLOT, R, FBYTES);
    printf("==========================================\n");
    return ok?0:2;
}
