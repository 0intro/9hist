/*
 *  all register offsets are relative to 0x8000000 so that
 *  IOZERO can be changed at some future point
 */
#define IOA(t, x)	((t*)(IOZERO|x))
