extern	Qinfo 	 urpinfo;
extern	Qinfo 	 asyncinfo;

void streaminit0(void){
	newqinfo(&urpinfo);
	newqinfo(&asyncinfo);
}
