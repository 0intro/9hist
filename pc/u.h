#define nil		((void*)0)
typedef	unsigned short	ushort;
typedef	unsigned char	uchar;
typedef	signed char	schar;
typedef unsigned int	uint;
typedef unsigned long	ulong;
typedef	 long long	vlong;
typedef union Length	Length;
typedef ushort		Rune;

union Length
{
	char	clength[8];
	vlong	vlength;
	struct{
		long	hlength;
		long	length;
	};
};

typedef char *va_list;

#define va_start(list, start) list = (sizeof(start)<4 ? (char *)((int *)&(start)+1) : \
(char *)(&(start)+1))
#define va_end(list)
#define va_arg(list, mode) ((mode*)(list += sizeof(mode)))[-1]
