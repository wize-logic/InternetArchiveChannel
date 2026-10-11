/* scandir() for devkitPPC newlib, which declares it in <dirent.h> but has no implementation. */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int scandir(const char *dir, struct dirent ***namelist, int (*filter)(const struct dirent *),
	int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *d = opendir(dir);
	if (!d)
		return -1;
	struct dirent **list = NULL, *e;
	size_t n = 0, cap = 0;
	while ((e = readdir(d)) != NULL) {
		if (filter && !filter(e))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			struct dirent **grown = realloc(list, cap * sizeof *list);
			if (!grown)
				goto fail;
			list = grown;
		}
		list[n] = malloc(sizeof *e);
		if (!list[n])
			goto fail;
		memcpy(list[n++], e, sizeof *e);
	}
	closedir(d);
	if (compar && n > 1)
		qsort(list, n, sizeof *list, (int (*)(const void *, const void *))compar);
	*namelist = list;
	return (int)n;
fail:
	while (n)
		free(list[--n]);
	free(list);
	closedir(d);
	errno = ENOMEM;
	return -1;
}
