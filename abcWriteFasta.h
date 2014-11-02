
class abcWriteFasta:public abc{
private:
  kstring_t bufstr;
  int currentChr;
  int NbasesPerLine;
  double lphred[256];//these are log phread scores log(10^(1:255)/(-10))
  char *myFasta;//contains the new fastasequence for the currrent chr
public:
  int doFasta;
  gzFile outfileZ;
  int doCount;

  abcWriteFasta(const char *outfiles,argStruct *arguments,int inputtype);
  ~abcWriteFasta();
  void changeChr(int refId);
  void getOptions(argStruct *arguments);
  void run(funkyPars  *pars);
  void print(funkyPars *pars);
  void clean(funkyPars *pars);
  void printArg(FILE *argFile);

};
