#include <stdio.h>
/* chars in megabyte */
#define MBCHAR 1048576

int i,j;
unsigned char storage[1024][MBCHAR];
int main()
{
	int i=0;
	while (1)
	{
		for (j=0;j<MBCHAR;j++)
		{
			storage[i][j] = 1;
		}
		i++;
		printf("allocated %d mb\n", i);
//		*a=(char*) malloc(MBCHAR*sizeof(char));
//		printf("allocd");
	}
	return 0;
}
