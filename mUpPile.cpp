#include <vector>
#include <cstring>
#include <cstdlib>
#include <ctype.h>
#include <pthread.h>
#include <cassert>
#include <htslib/hts.h>
#include "tpooled_alloc.h"
#include "mUpPile.h"
#include "abcGetFasta.h"
#include "analysisFunction.h"
#include "makeReadPool.h"
#include "pop1_read.h"

//#define __WITH_POOL__

extern int SIG_COND;
extern int minQ;
extern int trim;
#define bam_nt16_rev_table seq_nt16_str

#define MAX_SEQ_LEN 200 //this is used for getting some secure value for when a site is securely covered for sample
/*
  When merging different nodes, we are using a greedy but fast approach,
  however if we have a large genomic region with no data we are are allocating huge chunks.
  So the cutoff below will make a fallback to a slower but memsecure version
 */
#define BUG_THRES 1000000 //<- we allow 1mio sites to be allocated,
#define UPPILE_LEN 8

#ifdef __WITH_POOL__
tpool_alloc_t *nodes = NULL;
#endif

/*
  node for uppile for a single individual for a single site
  this is used for textoutput emulating samtools mpileup
  this is only kept for debugging purposes.

 */

typedef struct{
  int len;//length of seq,qs,pos
  kstring_t seq;
  kstring_t qs;
  kstring_t pos;
  int depth;
  int refPos;
}node;

template<typename T>
T getmax(const T *ary,size_t len){
  assert(len>0&&ary!=NULL);
      
  T high = ary[0];
  for(size_t i=1;i<len;i++){
    if(ary[i]>high)
      high=ary[i];
  }
  return high;
}

void dalloc_node(node *n){

#ifdef __WITH_POOL__
  tpool_free(nodes,n);
#else
  free(n->seq.s);
  free(n->qs.s);
  free(n->pos.s);
  free(n);
#endif
}


void dalloc_node(tNode *n){
  if(n->l!=0||n->m!=0){
    free(n->seq);
    free(n->qs);
    free(n->posi);
    free(n->mapQ);
    free(n->isop);
  }
}

typedef struct{
  char *refName;
  int regStart;
  int regStop;
  int nSites;
  int nSamples;
  node ***nd;//nd[site][ind]
  int *refPos;//length is nSites
  int first;
  int last;
  int start;
  int length;
  int refId;
}chunky;



void printChunky2(const chunky* chk,FILE *fp,char *refStr,abcGetFasta *gf) {
  //  fprintf(stderr,"[%s] nsites=%d region=(%d,%d) itrReg=(%d,%d)\n",__FUNCTION__,chk->nSites,chk->refPos[0],chk->refPos[chk->nSites-1],chk->regStart,chk->regStop);
  if(chk->refPos[0]>chk->regStop){
    fprintf(stderr,"\t->Problems with stuff\n");
    exit(0);
  }
  int refId = chk->refId;
  for(int s=0;s<chk->nSites;s++) {
    if(chk->refPos[s]<chk->regStart || chk->refPos[s]>chk->regStop-1 ){
      for(int i=0;i<chk->nSamples;i++)
	if(chk->nd[s][i])
	  dalloc_node(chk->nd[s][i]);
      free(chk->nd[s]);
      continue;
    }
    fprintf(fp,"%s\t%d",refStr,chk->refPos[s]+1);     
    if(gf->ref!=NULL){
      if(refId!=gf->ref->curChr)
	gf->loadChr(gf->ref,refStr,refId);
      if(gf->ref->seqs!=NULL){
	fprintf(fp,"\t%c",gf->ref->seqs[chk->refPos[s]]);
      }
    }
    for(int i=0;i<chk->nSamples;i++) {

      //      fprintf(stderr,"seqlen[%d,%d]=%lu\t",s,i,chk->nd[s][i].seq->l);
      if(chk->nd[s][i]){
	fprintf(fp,"\t%d\t",chk->nd[s][i]->depth);
	for(size_t l=0;l<chk->nd[s][i]->seq.l;l++)
	  fprintf(fp,"%c",chk->nd[s][i]->seq.s[l]);
	fprintf(fp,"\t");
		
	for(size_t l=0;l<chk->nd[s][i]->qs.l;l++)
	  fprintf(fp,"%c",chk->nd[s][i]->qs.s[l]);
	//	fprintf(fp,"\t");
	dalloc_node(chk->nd[s][i]);
      }else
	fprintf(fp,"\t0\t*\t*");
      
    }
    //    fprintf(stderr,"\n");
    fprintf(fp,"\n");
    free(chk->nd[s]);
  }
  delete [] chk->nd;
  delete [] chk->refPos;
  delete chk;
} 


typedef struct{
  int l;//number of nodes in nodes
  int m; //possible number of of nodes
  int first;//simply a value which is equal to nodes[0].refPos;
  int last;
  node **nds;//this length can maximum be the maxlenght of a read.NOTANYMORE

}nodePool;


nodePoolT *tndPool_initl(int l){
  nodePoolT *np = (nodePoolT*) calloc(1,sizeof(nodePoolT));
  np->m = l;
  kroundup32(np->m);
  np->l = 0;
  np->nds = new tNode*[np->m];
  np->nds = (tNode**) memset(np->nds,0,np->m*sizeof(tNode*));
  return np;
}


nodePool *ndPool_initl(int l){
  nodePool *np = (nodePool*) calloc(1,sizeof(nodePool));
  np->m = l;
  kroundup32(np->m);
  np->l = 0;
  np->nds = new node*[np->m];
  np->nds = (node**) memset(np->nds,0,np->m*sizeof(node*));
  return np;
}


void realloc(nodePool *np,int l){
  if(l>=np->m-4){
    delete [] np->nds;
    np->m = l;
    kroundup32(np->m);
    np->nds = new node*[np->m];  
  }
  np->nds = (node**) memset(np->nds,0,np->m*sizeof(node*));
  np->l=0;
  np->first=np->last=-1;
  
}


void realloc(nodePoolT *np,int l){
  if(l>=np->m-4){
    delete [] np->nds;
    np->m = l;
    kroundup32(np->m);
    np->nds = new tNode*[np->m];  
  }
  np->nds = (tNode**) memset(np->nds,0,np->m*sizeof(node*));
  np->l=0;
  np->first=np->last=-1;
  
}


void dalloc_nodePool (nodePool* np){
  for(int i=0;i<np->l;i++){
    fprintf(stderr,"np[i]:%d\n",i);
    dalloc_node(np->nds[i]);
  }
}

void dalloc_nodePoolT (nodePoolT* np){
  for(int i=0;i<np->l;i++)
    dalloc_node(np->nds[i]);
  
}

 

void cleanUpChunkyT(chunkyT *chk){
  for(int s=0;s<chk->nSites;s++) {
    for(int i=0;i<chk->nSamples;i++) {
      if(chk->nd[s][i]->l2!=0)
	for(int j=0;j<chk->nd[s][i]->l2;j++){
	  dalloc_node(&chk->nd[s][i]->insert[j]);//DRAGON
	}
      free(chk->nd[s][i]->insert);
      dalloc_node(chk->nd[s][i]);
    }
    delete [] chk->nd[s];
  }
  delete [] chk->nd;
  delete [] chk->refPos;
  delete chk;
  
}

node* node_init1(){
#ifdef __WITH_POOL__
  {
    node *nd = (node*)tpool_alloc(nodes);
    nd->seq.l=0;
    nd->qs.l=0;
    nd->pos.l=0;
    nd->refPos = -999;
    nd->depth =0;
    //notice we only set the l to zero, not m. This way we will avoid expensize reallocs in the kstring_t
    return nd;
  }
#else
  return (node*)calloc(1,sizeof(node));
#endif
}


tNode initNodeT(int l){
  tNode d;
  d.l = d.l2 = d.m2 = 0;
  d.m = l;
  kroundup32(d.m);
  if(d.m==0){
    d.seq=d.qs=NULL;
    d.posi=d.isop=d.mapQ=NULL;
  }else{
    d.seq=(char *)malloc(d.m);
    d.qs=(char *)malloc(d.m);
    d.posi=(unsigned char *)malloc(d.m);
    d.isop=(unsigned char *)malloc(d.m);
    d.mapQ=(unsigned char *)malloc(d.m);
  }
  d.refPos= -999;
  d.insert = NULL;
  return d;
}

tNode *tNode_init1(){
  fprintf(stderr,"alloc\n");
  tNode *d =  (tNode*)calloc(1,sizeof(tNode));
  d->m =1;
  d->l=d->l2=d->m2=0;

  if(d->m==0){
    d->seq=d->qs=NULL;
    d->posi=d->isop=d->mapQ=NULL;
  }else{
    d->seq=(char *)malloc(d->m);
    d->qs=(char *)malloc(d->m);
    d->posi=(unsigned char *)malloc(d->m);
    d->isop=(unsigned char *)malloc(d->m);
    d->mapQ=(unsigned char *)malloc(d->m);
  }
  d->refPos= -999;
  d->insert = NULL;
  
  return d;
}

tNode *tNode_init1(int l){
  tNode *d =  (tNode*)calloc(1,sizeof(tNode));

  d->l = d->l2 = d->m2 = 0;
  d->m = l;
  kroundup32(d->m);
  if(d->m==0){
    d->seq=d->qs=NULL;
    d->posi=d->isop=d->mapQ=NULL;
  }else{
    d->seq=(char *)malloc(d->m);
    d->qs=(char *)malloc(d->m);
    d->posi=(unsigned char *)malloc(d->m);
    d->isop=(unsigned char *)malloc(d->m);
    d->mapQ=(unsigned char *)malloc(d->m);
  }
  d->refPos= -999;
  d->insert = NULL;
  return d;
  
}

void realloc(char **c,int l,int m){
  char *tmp =(char *) malloc(m);
  memcpy(tmp,*c,l);
  free(*c);
  *c= tmp;
}

void realloc(unsigned char **c,int l,int m){
  unsigned char *tmp =(unsigned char *) malloc(m);
  memcpy(tmp,*c,l);
  free(*c);
  *c= tmp;
}
void realloc(tNode *d,int newsize){
  kroundup32(newsize);
  if(newsize<=d->m)
    fprintf(stderr,"[%s] problems newsize should be bigger than oldesize\n",__FUNCTION__);
  d->m=newsize;
  realloc(&(d->seq),d->l,d->m);
  realloc(&(d->qs),d->l,d->m);
  realloc(&(d->posi),d->l,d->m);
  realloc(&(d->isop),d->l,d->m);
  realloc(&(d->mapQ),d->l,d->m);
}


/*
  What does this do?
  returns the number of basepairs covered by np and sgl
*/

int coverage_in_bp(nodePool *np, readPool *sgl){
#if 0
  for(int i=0;0&&i<sgl->readIDstop;i++)
    fprintf(stderr,"sglrange[%d]\t %d\t%d\n",i,sgl->first[i],sgl->last[i]);
#endif
  int sumReg =0;
  int start=0;
  int first = np->first;
  int last = np->last +1;//because we are comparing with calc_end, which is noncontain

  if(np->l==0&&sgl->readIDstop!=0){//LAST PART OF CONDTIONAL WAS A MINDBUGGING BUG

    first = sgl->first[0];
    last = sgl->last[0];
    start++;
  }
  if(np->l==0&&sgl->readIDstop==0)//JESUS CHRIST
    return 0;
  for(int i=start;i<sgl->readIDstop;i++) {
    if(sgl->first[i]>last){
      sumReg += last-first;
      first =sgl->first[i];
    }
    last = std::max(sgl->last[i],last);
  }
  sumReg += last-first;
  return sumReg;
}

int coverage_in_bpT(nodePoolT *np, readPool *sgl){
#if 0
  for(int i=0;0&&i<sgl->readIDstop;i++)
    fprintf(stderr,"sglrange[%d]\t %d\t%d\n",i,sgl->first[i],sgl->last[i]);
#endif
  int sumReg =0;
  int start=0;
  int first = np->first;
  int last = np->last +1;//because we are comparing with calc_end, which is noncontain
  
  if(np->l==0&&sgl->readIDstop!=0){//LAST PART OF CONDTIONAL WAS A MINDBUGGING BUG

    first = sgl->first[0];
    last = sgl->last[0];
    start++;
  }
  if(np->l==0&&sgl->readIDstop==0)//JESUS CHRIST
    return 0;
  for(int i=start;i<sgl->readIDstop;i++) {
    if(sgl->first[i]>last){
      sumReg += last-first;
      first =sgl->first[i];
    }
    last = std::max(sgl->last[i],last);
  }
  sumReg += last-first;
  return sumReg;
}

void mkNodes_one_sampleb(readPool *sgl,nodePool *done_nodes,nodePool *old,abcGetFasta *gf) {
  int regionLen = coverage_in_bp(old,sgl);//true covered regions

  done_nodes->l =0;
  if(regionLen==0)
    return;
  int lastSecure = sgl->lowestStart;
  
  //done_nodes = allocNodePool(regionLen);
  realloc(done_nodes,regionLen);
  int offs = old->first;//first position from buffered
  int last = old->last+1;//because we are comparing with calc_end

  //plug in the old buffered nodes
  for(int i=0;i<old->l;i++)
    done_nodes->nds[old->nds[i]->refPos-offs] = old->nds[i];

  if(old->l==0){//if we didone_nodes't have have anybuffered then
    offs = sgl->first[0];
    last = sgl->last[0];
  }

  //parse all reads
  int r;
  for(r=0;r<sgl->readIDstop;r++) {
    bam1_t *rd=sgl->reads[r];

    //    fprintf(stderr,"r=%d\tpos=%d\n",r,rd.pos);
    if(sgl->first[r] > last){
      int diffs = (rd->core.pos-last);
      offs = offs + diffs;
      last = sgl->last[r];
    }else
      last = std::max(sgl->last[r],last);

    char *seq =(char *) bam_get_seq(rd);
    char *quals =(char *) bam_get_qual(rd);
    int nCig = rd->core.n_cigar;

    uint32_t *cigs = bam_get_cigar(rd);
    int seq_pos =0; //position within sequence
    int wpos = rd->core.pos-offs;//this value is the current position assocatied with the positions at seq_pos
    node *tmpNode =NULL; //this is a pointer to the last node beeing modified
    int hasInfo =0;//used when first part is a insertion or deletion
    int hasPrintedMaq =0;
    //loop through read by looping through the cigar string
    for(int i=0;i<nCig;i++) {
      int opCode = cigs[i]&BAM_CIGAR_MASK; //what to do
      int opLen = cigs[i]>>BAM_CIGAR_SHIFT; //length of what to do

      if(opCode==BAM_CINS||opCode==BAM_CDEL){//handle insertions and deletions
	if(i==0){ //skip indels if beginning of a read, print mapQ
	  tmpNode = done_nodes->nds[wpos]?done_nodes->nds[wpos]:(done_nodes->nds[wpos]=node_init1()); 
	  kputc('^', &tmpNode->seq);
	  if(rd->core.qual!=255)
	    kputc(rd->core.qual+33, &tmpNode->seq);
	  else
	    kputc('~', &tmpNode->seq);
	  hasPrintedMaq =1;
	  if(opCode==BAM_CINS){
	    seq_pos += opLen;
	    continue;
	  } 
	}
	if(opCode==BAM_CINS&&hasInfo==0){//when should this happen?
	  seq_pos += opLen;	  
	  continue;
	}
	if(i!=0){
	  wpos--; //insertion/deletion is bound to the last position of the read
	  tmpNode = done_nodes->nds[wpos]?done_nodes->nds[wpos]:(done_nodes->nds[wpos]=node_init1());
	  kputc(opCode&BAM_CINS?'+':'-',&tmpNode->seq);
	  kputw(opLen,&tmpNode->seq);
	  hasInfo++;
	}
	if(opCode==BAM_CINS){
	  for(int ii=0;ii<opLen;ii++){
	    char c = bam_nt16_rev_table[bam_seqi(seq, seq_pos)];
	    kputc(bam_is_rev(rd)? tolower(c) : toupper(c), &tmpNode->seq);
	    kputw(seq_pos+1,&tmpNode->pos);
	    kputc(',',&tmpNode->pos);
	    seq_pos++;
	  }
	  wpos++;
	}else {//this is the deletion part
	  if(i!=0){
	    if(gf->ref==NULL)
	      for(int ii=0;ii<opLen;ii++)
		kputc(bam_is_rev(rd)? tolower('N') : toupper('N'),&tmpNode->seq);
	    else
	      for(int ii=0;ii<opLen;ii++)
		kputc(bam_is_rev(rd)? tolower(gf->ref->seqs[offs+wpos+ii+1]) : toupper(gf->ref->seqs[offs+wpos+ii+1]),&tmpNode->seq);
	    wpos++;//write '*' from the next position and opLen more
	  }
	  for(int fix=wpos;wpos<fix+opLen;wpos++){
	  tmpNode = done_nodes->nds[wpos]?done_nodes->nds[wpos]:(done_nodes->nds[wpos]=node_init1());
	  //    tmpNode = &done_nodes.nds[wpos];
	    tmpNode->refPos=wpos+offs;
	    tmpNode->depth ++;
	    kputc('*',&tmpNode->seq);
	    kputw(seq_pos+1, &tmpNode->pos);
	    kputc(quals[seq_pos]+33, &tmpNode->qs);
	  }
	}

      }else if(opCode==BAM_CSOFT_CLIP){
	//occurs only at the ends of the read
	if(seq_pos == 0){
	  //then we are at beginning of read and need to write mapQ
	  tmpNode = done_nodes->nds[wpos]?done_nodes->nds[wpos]:(done_nodes->nds[wpos]=node_init1());
	  tmpNode->refPos=wpos+offs;
	  kputc('^', &tmpNode->seq);
	  if(rd->core.qual!=255)
	    kputc(rd->core.qual+33, &tmpNode->seq);
	  else
	    kputc('~', &tmpNode->seq);
	  seq_pos += opLen;
	  //	  wpos -= opLen;
	}else//we are at the end of read, then break CIGAR loop
	  break;
      }else if(opCode==BAM_CMATCH||opCode==BAM_CEQUAL||opCode==BAM_CDIFF) {
	hasInfo++;
	for(int fix=wpos ;wpos<(fix+opLen) ;wpos++) {
	  //	  fprintf(stderr,"wpos:%d done_nodes_>dns:%d\n",wpos,done_nodes->m);
	  tmpNode = done_nodes->nds[wpos]?done_nodes->nds[wpos]:(done_nodes->nds[wpos]=node_init1());
	  //  tmpNode =  &done_nodes.nds[wpos];
	  tmpNode->refPos=wpos+offs;
	  tmpNode->depth++;
	  if(seq_pos==0 &&hasPrintedMaq==0){
	    kputc('^', &tmpNode->seq);
	    if(rd->core.qual!=255)
	      kputc(rd->core.qual+33, &tmpNode->seq);
	    else
	      kputc('~', &tmpNode->seq);
	  }
	  char c = bam_nt16_rev_table[bam_seqi(seq, seq_pos)];

	  if(gf->ref==NULL ||gf->ref->chrLen<wpos+offs)//prints the oberved allele
	    kputc(bam_is_rev(rd)? tolower(c) : toupper(c), &tmpNode->seq);
	  else{
	    if(refToInt[c]==refToInt[gf->ref->seqs[wpos+offs]])
	      kputc(bam_is_rev(rd)? ',' : '.', &tmpNode->seq);
	    else
	      kputc(bam_is_rev(rd)? tolower(c) : toupper(c), &tmpNode->seq);
	  }
	  kputc(quals[seq_pos]+33, &tmpNode->qs);
	  kputw(seq_pos+1, &tmpNode->pos);
	  kputc(',',&tmpNode->pos);
	  tmpNode->len++;
	  seq_pos++;
	}
      }else if(opCode==BAM_CREF_SKIP) {
	  for(int fix=wpos;wpos<fix+opLen;wpos++){
	    tmpNode = done_nodes->nds[wpos]?done_nodes->nds[wpos]:(done_nodes->nds[wpos]=node_init1());
	    //  tmpNode = &done_nodes.nds[wpos];
	    tmpNode->refPos=wpos+offs;
	    tmpNode->depth ++;
	    bam_is_rev(rd)?kputc('<',&tmpNode->seq):kputc('>',&tmpNode->seq);
	    kputw(seq_pos+1, &tmpNode->pos);
	    kputc(quals[seq_pos]+33, &tmpNode->qs);
	  }
      }else if(opCode==BAM_CPAD||opCode==BAM_CHARD_CLIP) {
	//dont care
      }else{
	fprintf(stderr,"Problem with unsupported CIGAR opCode=%d\n",opCode);
      }
    }
    //after end of read/parsing CIGAR always put the endline char
    //  fprintf(stderr,"printing endpileup for pos=%d\n",rd->pos);
    kputc('$', &tmpNode->seq);
    bam_destroy1(rd);
  }

  //plug the reads back up //FIXME maybe do list type instead

  int miss= sgl->l-r;
  for(int i=0;i<(sgl->l-r);i++){
    sgl->reads[i] =sgl->reads[i+r];
    sgl->first[i] =sgl->first[i+r];
    sgl->last[i] =sgl->last[i+r];
  }
  sgl->l = miss;
  //copy the part not meant for printing in this round into the buffer



  int lastSecureIndex =regionLen;
  //fprintf(stderr,"lastSecureIndex=%d\tregionLen=%d\t ret->nds.refpos=%d\n",lastSecureIndex,regionLen,ret->nds[regionLen-1].refPos);
  int tailPos = done_nodes->nds[regionLen-1]->refPos;
  if(tailPos>lastSecure)
    lastSecureIndex = regionLen-tailPos+lastSecure;
  done_nodes->l = lastSecureIndex;
  
  if(regionLen-lastSecureIndex+4>old->m){
    delete [] old->nds;
    old->m=regionLen;
    kroundup32(old->m);
    old->nds = new node*[old->m];
  }
  old->l=0;
  assert(regionLen-lastSecureIndex+4<=old->m);
  for(int i=lastSecureIndex;i<regionLen;i++)
    old->nds[old->l++] = done_nodes->nds[i];


  if(old->l!=0){
    old->first = old->nds[0]->refPos;
    old->last = old->nds[old->l-1]->refPos;
  }

  if(done_nodes->l!=0){
    done_nodes->first = done_nodes->nds[0]->refPos;
    done_nodes->last = done_nodes->nds[done_nodes->l-1]->refPos;
  }
}



void mkNodes_one_sampleTb(readPool *sgl,nodePoolT *dn,nodePoolT *np) {
  int regionLen = coverage_in_bpT(np,sgl);//true covered regions
  //nodePoolT dn;
  dn->l =0;
  if(regionLen==0)
    return;
  int lastSecure = sgl->lowestStart;

  realloc(dn,regionLen);
    
  int offs = np->first;//first position from buffered
  int last = np->last+1;//because we are comparing with calc_end

  //plug in the old buffered nodes
  for(int i=0;i<np->l;i++)
    dn->nds[np->nds[i]->refPos-offs] = np->nds[i];

  if(np->l==0){//if we didn't have have anybuffered then
    offs = sgl->first[0];
    last = sgl->last[0];
  }

  //parse all reads
  int r;
  
  for( r=0;r<sgl->readIDstop;r++) {

    bam1_t *rd = sgl->reads[r];

    int mapQ = rd->core.qual;
    if(mapQ>=255) mapQ = 20;

    if(sgl->first[r] > last){
      int diffs = (rd->core.pos-last);
      offs = offs + diffs;
      last = sgl->last[r];
    }else
      last = std::max(sgl->last[r],last);

    char *seq =(char *) bam_get_seq(rd);
    char *quals =(char *) bam_get_qual(rd);
    int nCig = rd->core.n_cigar;

    uint32_t *cigs = bam_get_cigar(rd);
    int seq_pos =0; //position within sequence
    int wpos = rd->core.pos-offs;//this value is the current position assocatied with the positions at seq_pos
    tNode *tmpNode =NULL; //this is a pointer to the last node beeing modified
    int hasInfo = 0;
    //loop through read by looping through the cigar string
    for(int i=0;i<nCig;i++) {
      int opCode = cigs[i]&BAM_CIGAR_MASK; //what to do
      int opLen = cigs[i]>>BAM_CIGAR_SHIFT; //length of what to do
      //fprintf(stderr,"opCode=%d opLen=%d seqPos=%d wpos=%d\n",opCode,opLen,seq_pos,wpos);
      if(opCode==BAM_CINS||opCode==BAM_CDEL){//handle insertions and deletions
	if(opCode==BAM_CINS&&i==0){ //skip indels if beginning of a read
	  seq_pos += opLen;	  
	  continue;
	}
	if(opCode==BAM_CINS&&hasInfo==0){
	  seq_pos += opLen;	  
	  continue;
	}
	
	hasInfo++;
	if(opCode&BAM_CINS){
	  wpos--; //insertion/deletion is bound to the last position of the read
	  tmpNode = dn->nds[wpos]?dn->nds[wpos]:(dn->nds[wpos]=tNode_init1()); 
	  //tmpNode = &nds[wpos];  
	  if(tmpNode->l2 >= tmpNode->m2){
	    tmpNode->m2++;
	    kroundup32(tmpNode->m2);
	    tNode *dddd =(tNode *) malloc(tmpNode->m2*sizeof(tNode));
	    for(int ddddd=0;ddddd<tmpNode->l2;ddddd++)
	      dddd[ddddd] = tmpNode->insert[ddddd];
	    free(tmpNode->insert);
	    tmpNode->insert = dddd;
	  }
	  
	  tmpNode->insert[tmpNode->l2] = initNodeT(opLen);
	  for(int ii=0;ii<opLen;ii++){
	    char c = bam_nt16_rev_table[bam_seqi(seq, seq_pos)];
	    tmpNode->insert[tmpNode->l2].seq[ii] = bam_is_rev(rd)? tolower(c) : toupper(c);
	    tmpNode->insert[tmpNode->l2].posi[ii] = seq_pos + 1;
	    tmpNode->insert[tmpNode->l2].isop[ii] =rd->core.l_qseq- seq_pos - 1;
	    tmpNode->insert[tmpNode->l2].qs[ii] = quals[seq_pos];
	    if( quals[seq_pos]<minQ || seq_pos + 1 < trim || rd->core.l_qseq- seq_pos - 1 < trim)  
	      tmpNode->insert[tmpNode->l2].seq[ii] =  bam_is_rev(rd)? tolower('n') : toupper('N');
	    tmpNode->insert[tmpNode->l2].mapQ[ii] = mapQ;
	    seq_pos++;// <- important, must be after macro
	  }
	  tmpNode->l2++;//incrementor!
	  wpos++;
	}else {//this is the deletion part
	  
	  for(int ii=0;ii<opLen;ii++){
	    tmpNode = dn->nds[wpos+ii]?dn->nds[wpos+ii]:(dn->nds[wpos+ii]=tNode_init1()); 
	    //tmpNode = &nds[wpos+ii];
	    tmpNode->refPos = wpos +ii + offs;
	    tmpNode->deletion ++;
	  }
	  wpos += opLen;
	}

      }else if(opCode==BAM_CSOFT_CLIP){
	//occurs only at the ends of the read
	if(seq_pos == 0){
	  //then we are at beginning of read and need to write mapQ
	  tmpNode = dn->nds[wpos]?dn->nds[wpos]:(dn->nds[wpos]=tNode_init1()); 
	  //tmpNode = &nds[wpos];
	  seq_pos += opLen;
	}else//we are at the end of read, then break CIGAR loop
	  break;
      }else if(opCode==BAM_CMATCH||opCode==BAM_CEQUAL||opCode==BAM_CDIFF) {
	hasInfo++;
	for(int fix=wpos ;wpos<(fix+opLen) ;wpos++) {
	  tmpNode = dn->nds[wpos]?dn->nds[wpos]:(dn->nds[wpos]=tNode_init1()); 
	  //tmpNode =  &nds[wpos];
	  tmpNode->refPos=wpos+offs;
	  if(tmpNode->l>=tmpNode->m){
	    tmpNode->m = tmpNode->m*2;
	    tmpNode->seq =(char *) realloc(tmpNode->seq,tmpNode->m);
	    tmpNode->qs =(char *) realloc(tmpNode->qs,tmpNode->m);
	    tmpNode->posi =(unsigned char *) realloc(tmpNode->posi,tmpNode->m);
	    tmpNode->isop =(unsigned char *) realloc(tmpNode->isop,tmpNode->m);
	    tmpNode->mapQ = (unsigned char *) realloc(tmpNode->mapQ,tmpNode->m);
	  }
	  

	  char c = bam_nt16_rev_table[bam_seqi(seq, seq_pos)];
	  tmpNode->seq[tmpNode->l] = bam_is_rev(rd)? tolower(c) : toupper(c);
	  tmpNode->qs[tmpNode->l] =  quals[seq_pos];
	
	  tmpNode->posi[tmpNode->l] = seq_pos;
	  tmpNode->isop[tmpNode->l] =rd->core.l_qseq- seq_pos-1;
	  tmpNode->mapQ[tmpNode->l] = mapQ;
	  if( quals[seq_pos]<minQ || seq_pos  < trim || rd->core.l_qseq - seq_pos - 1 < trim )
	    tmpNode->seq[tmpNode->l] = bam_is_rev(rd)? tolower('n') : toupper('N');
	  
	  tmpNode->l++;
	  seq_pos++;
	}
      }else if(opCode==BAM_CREF_SKIP) {
	  for(int fix=wpos;wpos<fix+opLen;wpos++){
	    tmpNode = dn->nds[wpos]?dn->nds[wpos]:(dn->nds[wpos]=tNode_init1()); 
	    //tmpNode = &nds[wpos];
	    tmpNode->refPos=wpos+offs;
	  }
      }else if(opCode==BAM_CPAD||opCode==BAM_CHARD_CLIP) {
	//dont care
      }else{
	fprintf(stderr,"Problem with unsupported CIGAR opCode=%d\n",opCode);
      }
      //      exit(0);
    }
    //after end of read/parsing CIGAR always put the endline char
    bam_destroy1(rd);
  }

  //plug the reads back up //FIXME maybe do list type instead

  int miss= sgl->l-r;
  for(int i=0;i<(sgl->l-r);i++){
    sgl->reads[i] =sgl->reads[i+r];
    sgl->first[i] =sgl->first[i+r];
    sgl->last[i] =sgl->last[i+r];
  }
  sgl->l = miss;
  //copy the part not meant for printing in this round into the buffer



  int lastSecureIndex =regionLen;
  int tailPos = dn->nds[regionLen-1]->refPos;
  if(tailPos>lastSecure)
    lastSecureIndex = regionLen-tailPos+lastSecure;
  dn->l = lastSecureIndex;
  
  if(regionLen-lastSecureIndex+4>np->m){
    delete [] np->nds;
    np->m=regionLen+4;
    kroundup32(np->m);
    np->nds = new tNode*[np->m];
  }
  assert(regionLen-lastSecureIndex+4<=np->m);
  //  fprintf(stderr,"np->m:%d\n",np->m)
  np->l=0;
  for(int i=lastSecureIndex;i<regionLen;i++)
    np->nds[np->l++] = dn->nds[i];


  if(np->l!=0){
    np->first = np->nds[0]->refPos;
    np->last = np->nds[np->l-1]->refPos;
  }

  if(dn->l!=0){
    dn->first = dn->nds[0]->refPos;
    dn->last = dn->nds[dn->l-1]->refPos;
    #if 0
    fprintf(stderr,"[%s] first=%d last=%d\n",__FUNCTION__,dn->first,dn->last);
    #endif
  }
  // return dn;
}



/*
  old function.
  from:= min(dn);
  to:=min(max{dn[1],dn[2],...})

 */


void get_span_all_samples(nodePool **dn,int nFiles,int &from,int &to){
#if 0 
  for(int i=0;i<nFiles;i++)
    fprintf(stderr,"[%s]i=%d (%d,%d)=%d\n",__FUNCTION__,i,dn[i].first,dn[i].last,dn[i].l);
#endif
  for(int i=0;i<nFiles;i++)
    if(dn[i]->l!=0){
      from = dn[i]->first;
      to = dn[i]->last;
      break;
    }
  
  for(int i=0;i<nFiles;i++){//might skip the 'i' used above
    if(dn[i]->l==0)
      continue;
    if(dn[i]->last>to)
      to = dn[i]->last;
    if(dn[i]->first<from)
      from = dn[i]->first;
  }
}


void get_span_all_samplesT(nodePoolT **dn,int nFiles,int &from,int &to){
  for(int i=0;i<nFiles;i++)
    if(dn[i]->l!=0){
      from = dn[i]->first;
      to = dn[i]->last;
      break;
    }
  
  for(int i=0;i<nFiles;i++){//might skip the 'i' used above
    if(dn[i]->l==0)
      continue;
    if(dn[i]->last>to)
      to = dn[i]->last;
    if(dn[i]->first<from)
      from = dn[i]->first;
  }
}


typedef std::map<int,tNode **> umapT; 

chunkyT *slow_mergeAllNodes_new(nodePoolT **dn,int nFiles){
  //  fprintf(stderr,"starting [%s]\n",__FUNCTION__);
  
  umapT ret;
  umapT::iterator it;
  for(int f=0;f<nFiles;f++) {
    nodePoolT *sm = dn[f];
    for(int l=0;l<sm->l;l++) {
      tNode **perSite = NULL;
      int thepos =sm->nds[l]->refPos;
      it=ret.find(thepos);
      if(it!=ret.end())
	perSite = it->second;
      else{
	perSite = new tNode*[nFiles];
	for(int ii=0;ii<nFiles;ii++){
	  perSite[ii]->l=perSite[ii]->l2=perSite[ii]->m=0;
	  perSite[ii]->insert =NULL;
	  perSite[ii]->refPos = thepos;
	}

	
	ret.insert(std::make_pair(thepos,perSite));
		
      }
      perSite[f] = sm->nds[l];
    }

  }
  //now we perform the backrool
  int nnodes = ret.size();
  int *refPos = new int [ret.size()];
  //  fprintf(stderr,"nnodes=%d\n",nnodes);
  chunkyT *chk =new chunkyT;  
  chk->nd = new tNode**[nnodes];
  int p=0;
  for(it = ret.begin();it!=ret.end();++it){
    chk->nd[p++] = it->second;
    refPos[p-1] = chk->nd[p-1][0]->refPos;
  }

  chk->nSamples=nFiles;
  chk->nSites=nnodes;
  chk->refPos = refPos;
  return chk;  
}


chunkyT *mergeAllNodes_new(nodePoolT **dn,int nFiles) {

  int *depth = NULL;
  int *refPos2 = NULL;
  tNode ***super = NULL;
  
  if(dn==NULL){//cleanup called after end of looping through all files.
    return NULL;
  }
  int first,last;
  get_span_all_samplesT(dn,nFiles,first,last);

  int rlen = last-first+1;
  assert(rlen>=0);
  if(rlen>BUG_THRES)
    return slow_mergeAllNodes_new(dn,nFiles);

  super = new tNode**[rlen];
  depth = new int[rlen];
  refPos2 = new int[rlen];
  
  memset(depth,0,rlen*sizeof(int));
  int offs = first;
  //looping through the different samples
  for(int n=0;n<nFiles;n++) {
    nodePoolT *sm = dn[n];
    int i;
    //looping through all nodes
    for( i=0;((i<sm->l)&&(sm->nds[i]->refPos <= std::min(sm->last,last) ));i++) {
      
      int posi = sm->nds[i]->refPos-offs;
      if(depth[posi]==0){
	//	fprintf(stderr,"posi=%d\n",posi);
	super[posi] = new tNode*[nFiles];
	for(int ii=0;ii<nFiles;ii++){
	  super[posi][ii]->l=super[posi][ii]->l2=super[posi][ii]->m=0;
	  super[posi][ii]->insert =NULL;
	  super[posi][ii]->refPos = posi+offs;
	  //super[posi][ii] = initNodeT(UPPILE_LEN,posi+offs,posi);
	}
	
	refPos2[posi]=sm->nds[i]->refPos;
      }
      depth[posi]++;
      super[posi][n] = sm->nds[i];
      //      super[posi][n]->len++;
    }
  }
  int nnodes =0;
  for(int i=0;i<rlen;i++)
    if(depth[i]!=0)
      nnodes++;
  
  //  fprintf(stderr,"YYYY nnnosed=%d\n",nnodes);
  chunkyT *chk =new chunkyT;  
  chk->nd = new tNode**[nnodes];
  int p=0;
  int *refPos = new int[nnodes];
  for(int i=0;i<nnodes;i++)
    refPos[i] = -9;
  for(int i=0;i<rlen;i++){
    if(depth[i]!=0){
      //   fprintf(stderr,"refpos2[%d]=%d\n",i,refPos2[i]);
      refPos[p] = refPos2[i];
      chk->nd[p++] = super[i];
    }
    //   delete [] super[i];
  }

  delete [] super;
  delete [] refPos2;
  delete [] depth;
  chk->nSamples=nFiles;
  chk->nSites=nnodes;
  chk->refPos = refPos;
  return chk;
}

pthread_mutex_t mUpPile_mutex = PTHREAD_MUTEX_INITIALIZER;


typedef std::map<int,node **> umap; 

chunky *slow_mergeAllNodes_old(nodePool **dn,int nFiles){
  //  fprintf(stderr,"starting [%s]\n",__FUNCTION__);
  
  umap ret;
  umap::iterator it;
  for(int f=0;f<nFiles;f++){
    nodePool *sm = dn[f];
    for(int l=0;l<sm->l;l++) {
      node **perSite = NULL;
      int thepos =sm->nds[l]->refPos;
      it=ret.find(thepos);
      if(it!=ret.end())
	perSite = it->second;
      else{
	perSite =(node**) calloc(nFiles,sizeof(node*));
	ret[thepos]=perSite;
		
      }
      perSite[f] = sm->nds[l];
    }
  }
  //now we perform the backrool
  int nnodes = ret.size();
  int *refPos = new int [ret.size()];
  //  fprintf(stderr,"nnodes=%d\n",nnodes);
  chunky *chk =new chunky;  
  chk->nd = new node**[nnodes];
  int p=0;
  for(it = ret.begin();it!=ret.end();++it){
    chk->nd[p++] = it->second;
    refPos[p-1] = chk->nd[p-1][0]->refPos;
  }

  chk->nSamples=nFiles;
  chk->nSites=nnodes;
  chk->refPos = refPos;
  chk->first = refPos[0];
  chk->last = refPos[nnodes-1];
  return chk;  
}




chunky *mergeAllNodes_old(nodePool **dn,int nFiles) {
  int first,last;
  get_span_all_samples(dn,nFiles,first,last);
  int rlen = last-first+1; 

  if(rlen>BUG_THRES) 
    return slow_mergeAllNodes_old(dn,nFiles);
  
  int depth[rlen];
  
  node ***super=NULL;
  try{
    //    fprintf(stderr,"rlen=%d\n",rlen);
    fflush(stderr);
    super = new node**[rlen];
  }
  catch(char *str){
    fprintf(stderr,"rlen=%d\n",rlen);
    fprintf(stderr,"problems allocating:%s\n",str);
  }
  int *refPos2 = new int[rlen];
  memset(depth,0,sizeof(int)*rlen);
  int offs = first;
  //looping through the different samples
  for(int n=0;n<nFiles;n++){
    nodePool *sm = dn[n];
    int i;
    //looping through all nodes
    for( i=0;((i<sm->l)&&(sm->nds[i]->refPos <= std::min(sm->last,last) ));i++) {
      
      int posi = sm->nds[i]->refPos-offs;
      if(depth[posi]==0){
	super[posi] =(node**) calloc(nFiles,sizeof(node*));
	refPos2[posi]=sm->nds[i]->refPos;
      }
      depth[posi]++;
      super[posi][n] = sm->nds[i];
      super[posi][n]->len++;
    }
    
  }
  int nnodes =0;
  for(int i=0;i<rlen;i++)
    if(depth[i]!=0)
      nnodes++;

  //  fprintf(stderr,"YYYY nnnosed=%d\n",nnodes);
  chunky *chk =new chunky;  
  chk->nd = new node**[nnodes];
  int p=0;
  int *refPos = new int[nnodes];
  for(int i=0;i<nnodes;i++)
    refPos[i] = -9;
  for(int i=0;i<rlen;i++){
    if(depth[i]!=0){
      //   fprintf(stderr,"refpos2[%d]=%d\n",i,refPos2[i]);
      refPos[p] = refPos2[i];
      chk->nd[p++] = super[i];
    }
    //   delete [] super[i];
  }
  //  fprintf(stderr,"nnodes=%d p=%d\n",nnodes,p);
  delete [] super;
  delete [] refPos2;

  chk->nSamples=nFiles;
  chk->nSites=nnodes;
  chk->refPos = refPos;
  chk->first = refPos[0];
  chk->last = refPos[nnodes-1];
  return chk;
}





int getSglStop5(readPool *sglp,int nFiles,int pickStop) {
#if 0
    fprintf(stderr,"[%s]\n",__FUNCTION__);
    for(int i=0;i<nFiles;i++){
      if(sglp[i].l!=0)
	fprintf(stderr,"ret.l=%d (%d,%d)\n",sglp[i].l,sglp[i].first[0],sglp[i].last[sglp[i].l-1]);
      else
	fprintf(stderr,"ret.l=%d (-1,-1)\n",sglp[i].l);
      for(int s=0;0&&s<sglp[i].l;s++)
	fprintf(stderr,"%d %d\n",sglp[i].first[s],sglp[i].last[s]);
    }

    fprintf(stderr,"\n");
#endif
    int lowestStart = pickStop ;
    if(lowestStart==-1){
      for(int i=0;i<nFiles;i++)
	if(sglp[i].l>0&&(getmax(sglp[i].first,sglp[i].l)<lowestStart))
	  lowestStart = getmax(sglp[i].first,sglp[i].l);
      assert(lowestStart!=pickStop);
    }

  lowestStart--;
  //fprintf(stderr,"loweststart=%d\n",lowestStart);
  /*
    now we looptrhough each readpool, and find 2 values.
    1) the readnumber of the last read to be included for uppiling,this
    2) the lastposition for which we know that the uppiling is complete
  */
  int tmp =0;
  for(int i=0;i<nFiles;i++){
    sglp[i].lowestStart = lowestStart;
    int j;
    for( j=0;j<sglp[i].l;j++)
      if(sglp[i].first[j]>lowestStart)
	break;
    sglp[i].readIDstop=j;
    tmp += j;
  }
  return tmp;
}


/*
  this function just returns the highest position along all buffered nodepools and readpools
 */

void getMaxMax2(readPool *sglp,int nFiles,nodePool **nps){
#if 0
    fprintf(stderr,"[%s].nFiles=%d\n",__FUNCTION__,nFiles);
    for(int i=0;i<nFiles;i++){
      fprintf(stderr,"%d=sglp.l=%d\n",i,sglp[i].l);
    }
#endif
    
    int last_bp_in_chr = -1;
    
    for(int i=0;i<nFiles;i++){
      readPool *tmp = &sglp[i];
      if(tmp->l>0 && (getmax(tmp->last,tmp->l)>last_bp_in_chr) )
	last_bp_in_chr = getmax(tmp->last,tmp->l);
      if(nps[i]->last>last_bp_in_chr)
	last_bp_in_chr = nps[i]->last;
    }
    //  assert(last_bp_in_chr!=-1); This assertion should not be active, since it might be true if we don't have data for a whole chromosomes
    for(int i=0;i<nFiles;i++){
      sglp[i].lowestStart = last_bp_in_chr;
      sglp[i].readIDstop=sglp[i].l;
    }
    
}

void getMaxMax2(readPool *sglp,int nFiles,nodePoolT **nps){
#if 0
  fprintf(stderr,"[%s].nFiles=%d\n",__FUNCTION__,nFiles);
  for(int i=0;i<nFiles;i++)
    fprintf(stderr,"%d=sglp.l=%d\n",i,sglp[i].l);

#endif

  int last_bp_in_chr = -1;

  for(int i=0;i<nFiles;i++){
    readPool *tmp = &sglp[i];
    if(tmp->l>0 && (getmax(tmp->last,tmp->l)>last_bp_in_chr) )
      last_bp_in_chr = getmax(tmp->last,tmp->l);
    if(nps[i]->last>last_bp_in_chr)
      last_bp_in_chr = nps[i]->last;
  }
  //  assert(last_bp_in_chr!=-1); This assertion should not be active, since it might be true if we don't have data for a whole chromosomes
  for(int i=0;i<nFiles;i++){
    sglp[i].lowestStart = last_bp_in_chr;
    sglp[i].readIDstop=sglp[i].l;
  }

}


void getOffsets(htsFile *fp,char *fn,const bam_hdr_t *hd,hts_idx_t **idx,hts_itr_t **itr,int ref,int start,int stop,bam_hdr_t *hdr){
  if(*idx==NULL)
    *idx = sam_index_load(fp,fn);
  if (idx == 0) { // index is unavailable
    fprintf(stderr, "[main_samview] random alignment retrieval only works for indexed BAM or CRAM files.\n");
    exit(0);
  }
  char tmp[1024];
  snprintf(tmp,1024,"%s:%d-%d",hd->target_name[ref],start+1,stop);
  if(*itr)
    hts_itr_destroy(*itr);
  *itr = sam_itr_querys(*idx, hdr, tmp);
  if (*itr == NULL) { // reference name is not found
    fprintf(stderr, "[main_samview] region \"%s\" specifies an unknown reference name. Continue anyway.\n",tmp);
    exit(0);
  }

}



void *setIterator1(void *args){
  bufReader *rd = (bufReader *) args;

  int start,stop,ref;
  start = rd->regions.start;
  stop = rd->regions.stop;
  ref = rd->regions.refID;
  getOffsets(rd->fp,rd->fn,rd->hdr,&rd->idx,&rd->itr,ref,start,stop,rd->hdr);//leak
  return EXIT_SUCCESS;
}


 void setIterator1_thread(void *args,int index){
   //   fprintf(stderr,"[%s]\n",__FUNCTION__);
  bufReader *rds = (bufReader *) args;
  bufReader *rd = &rds[index];
  // free(rd->it.off);
  int start,stop,ref;
  start = rd->regions.start;
  stop = rd->regions.stop;
  ref = rd->regions.refID;
  getOffsets(rd->fp,rd->fn,rd->hdr,&rd->idx,&rd->itr,ref,start,stop,rd->hdr);
}

void setIterators(bufReader *rd,regs regions,int nFiles,int nThreads){
  for(int i=0;1&&i<nFiles;i++){
    rd[i].regions=regions;
    if(nThreads==1)
      setIterator1(&rd[i]);
  }
  if(nThreads>1){
    pthread_t myT[nThreads];
    int cnt=0;
    while(cnt<nFiles){
      int nTimes;
      if(nFiles-cnt-nThreads>=0)
	nTimes = nThreads;
      else
	nTimes = nFiles-cnt;
      for(int i=0;0&&i<nTimes;i++)
	fprintf(stderr,"cnt:%d i:%d\n",cnt+i,i);
      for(int i=0;i<nTimes;i++)
	pthread_create(&myT[i],NULL,setIterator1,&rd[cnt+i]);
      for(int i=0;i<nTimes;i++)
	pthread_join(myT[i], NULL);

      cnt+=nTimes;
    }
  }
}


void callBack_bambi(fcb *fff);//<-angsd.cpp
//type=1 -> samtool mpileup textoutput
//type=0 -> callback to angsd

//function below is a merge between the original TESTOUTPUT and the angsdcallback. Typenames with T are the ones for the callback
//Most likely this can be written more beautifull by using templated types. But to lazy now.
int uppile(int show,int nThreads,bufReader *rd,int nLines,int nFiles,std::vector<regs> regions,abcGetFasta *gf) {
#ifdef __WITH_POOL__
  //  fprintf(stderr,"alloc pool\n");
  nodes = tpool_create(sizeof(node));
#endif  
  assert(nLines&&nFiles);
  fprintf(stderr,"\t-> Parsing %d number of samples \n",nFiles);fflush(stderr);
  
  if(show!=1)
    pthread_mutex_lock(&mUpPile_mutex);//just to make sure, its okey to clean up
  extern abcGetFasta *gf;//<- only used for textoutput

  readPool *sglp= new readPool[nFiles];//<- one readpool per bam/cram
  
  nodePool **old_nodes,**done_nodes;
  old_nodes = done_nodes = NULL;

  nodePoolT **old_nodesT,**done_nodesT;
  old_nodesT = done_nodesT = NULL;
  
  //nodePoolT *npsT = NULL;
  if(show){
    old_nodes = new nodePool*[nFiles];// <- buffered nodes
    done_nodes = new nodePool*[nFiles];//<-nodes that are done
  }else{
    old_nodesT = new nodePoolT*[nFiles];// <- buffered nodes
    done_nodesT = new nodePoolT*[nFiles];//<-contains the finished notes
  }
  for(int i=0;i<nFiles;i++){
    sglp[i] = makePoolb(nLines);
    //sglpb[i] = makePoolb(nLines);
    if(show){
      old_nodes[i] = ndPool_initl(MAX_SEQ_LEN);
      done_nodes[i] = ndPool_initl(MAX_SEQ_LEN);
      //      fprintf(stderr,"%d) on.m:%d on.l:%d dn.m:%d dn.l:%d\n",i, old_nodes[i].m,old_nodes[i].l,done_nodes[i].m, done_nodes[i].l);
    }else{
      old_nodesT[i] = tndPool_initl(MAX_SEQ_LEN);
      done_nodesT[i] = tndPool_initl(MAX_SEQ_LEN);
    }
  }

  int itrPos = -1;
  int sumEof = 0;//sumof readerobject that has eof

  int theRef=-1;//<- this is current referenceId,
  while( SIG_COND) {

    int notDone = nFiles;
    sumEof =0;
    //reset the done region flag, while checking that any file still has data
    for(int i=0;i<nFiles;i++){
      sumEof += rd[i].isEOF;
      rd[i].regionDone =0;
    }
#if 0
    for(int i=0;i<nFiles;i++)
      fprintf(stderr,"[%s] i=%d eof=%d regionDone=%d\n",__FUNCTION__,i,rd[i].isEOF,rd[i].regionDone);
#endif

    //break loop if all files are done
    if(sumEof==nFiles&&regions.size()==0)
      break;

    if(regions.size()!=0) {
      if(itrPos+1==(int)regions.size()){
	break;
      }else {
	itrPos++;
	//fprintf(stderr,"region lookup %d/%lu\n",itrPos+1,regions.size());
	//fflush(stderr);
	setIterators(rd,regions[itrPos],nFiles,nThreads);
	//fprintf(stderr,"done region lookup %d/%lu\n",itrPos+1,regions.size());
	//fflush(stderr);///BAME

	theRef = rd[0].itr->tid; 
	//	fprintf(stderr,"theRef:%d %p %p\n",theRef,rd[0].it.off,rd[0].it.hts_itr->off);
	//validate we have offsets;
	int gotData = 0;
	for(int i=0;i<nFiles;i++)
	  if((rd[i].itr &&(rd[i].itr->off!=NULL))){
	    gotData =1;
	    break;
	  }
	//reset eof and region donepointers.
	for(int i=0;i<nFiles;i++){
	  rd[i].isEOF = 0;
	  rd[i].regionDone =0;
	  sglp[i].l =0;
	  sglp[i].bufferedRead=NULL;
	}
      }
    }else{
      if(theRef==-1){//init
	theRef =0;

      }else if(theRef==rd[0].hdr->n_targets-1){
	break;//then we are done
      }else{
	int minRef = rd[0].hdr->n_targets;
	for(int i=0;i<nFiles;i++)
	  if(sglp[i].bufferedRead && sglp[i].bufferedRead->core.tid<minRef)
	    minRef = sglp[i].bufferedRead->core.tid;
	if(minRef==-2||minRef == rd[0].hdr->n_targets){
	  theRef++;
	}else
	  theRef = minRef;
      }
    }

    if(theRef==rd[0].hdr->n_targets)
      break;
    assert(theRef>=0 && theRef<rd[0].hdr->n_targets);//ref should be inrange [0,#nref in header]

    //load fasta for this ref if needed
    void waiter(int);
    waiter(theRef);//will wait for exisiting threads and then load stuff relating to the chromosome=theRef;
    if(gf->ref!=NULL && theRef!=gf->ref->curChr)
      gf->loadChr(gf->ref,rd[0].hdr->target_name[theRef],theRef);
    
     

    //now we have changed chromosome we should plug in buffered, if buffered is same as the new chr
    //this is not needed if we use the indexing

    if(regions.size()==0){
      for(int i=0;i<nFiles;i++){
	extern abcGetFasta *gf;
	if(sglp[i].bufferedRead ==NULL)//<- no buffered no nothing
	  continue;
	if(sglp[i].bufferedRead->core.tid==theRef) {//buffered is on correct chr
	  int doCpy =1;
	  if(gf->ref!=NULL){
	    //this will modify the quality and mapQ if -baq >0 or adjustMapQ= SAMtools -c
	    doCpy = restuff(sglp[i].bufferedRead);
	  }
	  if(doCpy){
	    sglp[i].reads[sglp[i].l] = sglp[i].bufferedRead;
	    sglp[i].first[sglp[i].l] = sglp[i].reads[sglp[i].l]->core.pos;
	    sglp[i].last[sglp[i].l] =   bam_endpos(sglp[i].reads[sglp[i].l]);
	    sglp[i].l++;
	  }
	  //if we haven't performed the copy its because adjustedMap<minMapQ, in either case we don't need the read anymore
	  sglp[i].bufferedRead = NULL;
	}else // buffered was not on correct chr say that the 'region' is done
	  rd[i].regionDone=1;
      }
    }

    //below loop will continue untill eoc/oef/eor <- funky abbrev =end of chr, file and region
    while(notDone&& SIG_COND) {

      //before collecting reads from the files, lets first check if we should pop reads from the buffered queue in each sgl
      for(int i=0;i<nFiles;i++){
	if(sglp[i].bufferedRead&&sglp[i].bufferedRead->core.tid==theRef){
	  sglp[i].reads[sglp[i].l] = sglp[i].bufferedRead;
	  sglp[i].first[sglp[i].l] = sglp[i].reads[sglp[i].l]->core.pos;
	  sglp[i].last[sglp[i].l] = bam_endpos(sglp[i].reads[sglp[i].l]);
	  sglp[i].l++;
	  sglp[i].bufferedRead = NULL;
	}
	
      }
      
      int pickStop=-1;
      
      int doFlush = (collect_reads(rd,nFiles,notDone,sglp,nLines,theRef,pickStop)==nFiles)?1:0 ;

#if 0
      for(int i=0;i<nFiles;i++)
	fprintf(stderr,"[%s] sgl[%d].l=%d (%d,%d)\n",__FUNCTION__,i,sglp[i].l,sglp[i].first[0],sglp[i].last[sglp[i].l-1]);
      
#endif
      
      //prepare uppiling for all data, if end of chromosome or all files are EOF
      if(doFlush||notDone==0){
	//fprintf(stderr,"[%s]Last chunk Will flush all remaining reads\n",__FUNCTION__);
	if(show)
	  getMaxMax2(sglp,nFiles,old_nodes);
	else
	  getMaxMax2(sglp,nFiles,old_nodesT);
      }else{
	//getSglStop returns the sum of reads to parse.
	int hasData = getSglStop5(sglp,nFiles,pickStop);
	//	fprintf(stderr,"hasData=%d\n",hasData);
	if(hasData==0&&notDone!=0){
	  continue;
	}
      }

      //make uppile nodes
      
      //  nodePoolT *dnT = NULL;
      int tmpSum = 0;

      if(show!=1){
	//dnT = new nodePoolT[nFiles];//<- this can leak now
	for(int i=0;i<nFiles;i++){
	  mkNodes_one_sampleTb(&sglp[i],done_nodesT[i],old_nodesT[i]);
	  tmpSum += done_nodesT[i]->l;
	}
      }else
	for(int i=0;i<nFiles;i++) {
	  mkNodes_one_sampleb(&sglp[i],done_nodes[i],old_nodes[i],gf);
	  tmpSum += done_nodes[i]->l;
	}

#if 0
	for(int i=0;i<nFiles;i++)
	  if(show!=1)
	    fprintf(stderr,"[%s.%s():%d] l=%d first=%d last=%d\n",__FILE__,__FUNCTION__,__LINE__,done_nodes[i].l,done_nodes[i].first,done_nodes[i].last);
	  else
	    fprintf(stderr,"[%s.%s():%d] l=%d first=%d last=%d\n",__FILE__,__FUNCTION__,__LINE__,dnT[i].l,dnT[i].first,dnT[i].last);
#endif
            
      //simple sanity check for validating that we have indeed something to print.

      if(tmpSum==0){
	if(regions.size()!=0)
	  notDone=1;
	else{
	  fprintf(stderr,"No data for chromoId=%d chromoname=%s\n",theRef,rd[0].hdr->target_name[theRef]);
	  fprintf(stderr,"This could either indicate that there really is no data for this chromosome\n");
	  fprintf(stderr,"Or it could be problem with this program regSize=%lu notDone=%d\n",regions.size(),notDone);
	}
	//delete [] dnT;
	break;
      }

      int regStart,regStop;
      if(itrPos!=-1){
	regStart = regions[itrPos].start;
	regStop = regions[itrPos].stop;
      }else{
	regStart = -1;
	regStop = rd[0].hdr->target_len[theRef];
      }
      if(show){
	//merge the perFile upnodes.FIXME for large regions (with gaps) this will allocate to much...
	chunky *chk =mergeAllNodes_old(done_nodes,nFiles);
	assert(chk->refPos[0]<=regStop);
	
	chk->regStart = regStart;
	chk->regStop = regStop;
	chk->refName = rd[0].hdr->target_name[theRef];
	chk->refId = theRef;
	printChunky2(chk,stdout,chk->refName,gf);

      }else{

	fcb *f = new fcb; //<-for call back
	f->dn=done_nodesT; f->nFiles = nFiles; f->regStart = regStart; f->regStop = regStop; f->refId = theRef;
	callBack_bambi(f);
      }

      //if we flush, its due to end of chr/region or end of files
      if(doFlush)
	break;
    }

  }
  //below can be written nice but is just a copy paste, time is 10pm to lazy now
  if(show==0){
    callBack_bambi(NULL);//cleanup signal
    pthread_mutex_lock(&mUpPile_mutex);//just to make sure, its okey to clean up
    for(int i=0;1&&i<nFiles;i++){
      dalloc_nodePoolT(old_nodesT[i]);
      delete [] old_nodesT[i]->nds;
      dalloc(&sglp[i]);
    }
    delete [] old_nodesT;
    delete [] sglp;
    pthread_mutex_unlock(&mUpPile_mutex);//just to make sure, its okey to clean up
  }else{
    //clean up
    for(int i=0;i<nFiles;i++){
      dalloc(&sglp[i]);

      delete [] old_nodes[i]->nds;
      delete [] done_nodes[i]->nds;
      free(old_nodes[i]);
      free(done_nodes[i]);
    }

    delete [] old_nodes;
    delete [] done_nodes;
    delete [] sglp;
  }

#ifdef __WITH_POOL__
  for(int i=0;i<nodes->ntpools;i++){
    node *nd  =(node*)nodes->tpools[i].tpool;
    int nitem = 1024*1024/ sizeof(node);
    for(int j=0;j<nitem;j++){
      free(nd[j].seq.s);
      free(nd[j].qs.s);
      free(nd[j].pos.s);
    }
  }
  tpool_destroy(nodes);
#endif
  
  return 0;
}

