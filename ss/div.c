#include <u.h>

#define	W	32

ulong
_udiv(ulong dividend, ulong divisor)
{
	long R;		/* partial remainder */
	ulong Q;	/* partial quotient */
	int iter;
	ulong logQ, m;

	m = (1<<(W-1))+1;
	if(dividend < m)
		m = dividend;
	for(logQ=0; logQ<W; logQ++)
		if((divisor<<logQ) >= m)
			break;

	R = dividend;
	Q = 0;
	for(iter=logQ; iter>=0; iter--){
		if(R >= 0){
			R -= divisor<<iter;
			Q += 1<<iter;
		}else{
			R += divisor<<iter;
			Q -= 1<<iter;
		}
	}
	if(R < 0)
		Q--;
	return Q;
}

ulong
_urem(ulong dividend, ulong divisor)
{
	long R;		/* partial remainder */
	int iter;
	ulong logQ, m;

	m = (1<<(W-1))+1;
	if(dividend < m)
		m = dividend;
	for(logQ=0; logQ<W; logQ++)
		if((divisor<<logQ) >= m)
			break;

	R = dividend;
	for(iter=logQ; iter>=0; iter--){
		if(R >= 0)
			R -= divisor<<iter;
		else
			R += divisor<<iter;
	}
	if(R < 0)
		R += divisor;
	return R;
}

ulong
_div(long dividend, long divisor)
{
	if(dividend>=0 && divisor>=0)
		return _udiv(dividend, divisor);
	if(dividend<0 && divisor<0)
		return _udiv(-dividend, -divisor);
	if(dividend < 0)
		return -_udiv(-dividend, divisor);
	return -_udiv(dividend, -divisor);
}

ulong
_rem(long dividend, long divisor)
{
	if(dividend>=0 && divisor>=0)
		return _urem(dividend, divisor);
	if(dividend<0 && divisor<0)
		return _urem(-dividend, -divisor);
	if(dividend < 0)
		return -_urem(-dividend, divisor);
	return -_urem(dividend, -divisor);
}

long
_asmul(long *a, long b)
{
	long c;

	c = *a * b;
	*a = c;
	return c;
}

long
_asdiv(long *a, long b)
{
	long c;

	c = *a / b;
	*a = c;
	return c;
}

long
_asrem(long *a, long b)
{
	long c;

	c = *a % b;
	*a = c;
	return c;
}

ulong
_asudiv(ulong *a, ulong b)
{
	ulong c;

	c = *a / b;
	*a = c;
	return c;
}

ulong
_asurem(ulong *a, ulong b)
{
	ulong c;

	c = *a % b;
	*a = c;
	return c;
}
