/*
 * Copyright (c) 2005-2011 Atheme Project (http://www.atheme.org)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Description of config files parsed by this:
 *
 * configfile   = *WS *configentry
 * configentry  = value [1*WS value] [1*WS "{" *(configentry 1*WS) "}" ] *WS ";"
 * value        = 1*achar / DQUOTE *qchar DQUOTE
 * achar        = <any CHAR except WS or DQUOTE>
 * qchar        = <any CHAR except DQUOTE or \> / "\\" / "\" DQUOTE
 * comment      = "/" "*" <anything except * /> "*" "/" /
 *                "#" *CHAR %0x0A /
 *                "//" *CHAR %0x0A
 * WS           = %x09 / %x0A / %x0D / SPACE / "=" / comment
 *
 * A value of "include" for toplevel configentries causes a file to be
 * included. The included file is logically appended to the current file,
 * no matter where the include directive is. Include files must have balanced
 * braces.
 */
/*
 * Original idea from the csircd config parser written by Fred Jacobs
 * and Chris Behrens.
 */

#include "core.h"
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>

#define MAX_INCLUDE_NESTING 16
#define log printf
#define alloc malloc

static config_file_t *config_file_parse(const char *filename, char *confdata);
static void config_file_entry_free(config_file_entry_t *ceptr);
static config_file_t *config_file_load_internal(config_file_t *parent, const char *filename);

#define CF_ERRORED(cf) ((cf)->curline <= 0)

static void config_file_error(config_file_t *cf, const char *format, ...)
{
	va_list ap;
	char buffer[1024];
	char *ptr;

	va_start(ap, format);
	vsnprintf(buffer, sizeof buffer, format, ap);
	va_end(ap);

	if ((ptr = strchr(buffer, '\n')) != NULL)
		*ptr = '\0';

	if (cf != NULL)
	{
		if (cf->curline < 0)
			cf->curline = -cf->curline;

		log("%s:%d: %s",	cf->filename, cf->curline, buffer);

		/* mark config parse as failed */
		cf->curline = -cf->curline;
	}
	else
		log("config_file_parse(): %s", buffer);
}

static void skip_ws(char **pos, config_file_t *cf)
{
	int startline;

	for (;;)
	{
		switch (**pos)
		{
			case ' ':
			case '\t':
			case '\r':
			case '=': /* XXX */
				break;
			case '\n':
				cf->curline++;
				break;
			case '/':
				if ((*pos)[1] == '*')
				{
					startline = cf->curline;
					(*pos)++;
					(*pos)++;
					while (**pos != '\0' && (**pos != '*' || (*pos)[1] != '/'))
					{
						if (**pos == '\n')
							cf->curline++;
						(*pos)++;
					}
					if (**pos == '\0')
						config_file_error(cf, "File ends inside comment starting at line %d", startline);
					else
						(*pos)++; /* skip '*' */
				}
				else if ((*pos)[1] == '/')
				{
					while (**pos != '\0' && **pos != '\n' && **pos != '\r')
						(*pos)++;
					continue;
				}
				else
					return;
				break;
			case '#':
				while (**pos != '\0' && **pos != '\n' && **pos != '\r')
					(*pos)++;
				continue;
			default:
				return;
		}
		if (**pos == '\0')
			return;
		(*pos)++;
	}
}

static char *get_value(char **pos, config_file_t *cf, char *skipped)
{
	char *p = *pos;
	char *q;
	char *start;

	*skipped = '\0';
	if (*p == '"')
	{
		p++;
		start = p;
		q = p;
		while (*p != '\0' && *p != '\r' && *p != '\n' && *p != '"')
		{
			if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
				p++;
			*q++ = *p++;
		}
		if (*p == '\0')
		{
			config_file_error(cf, "File ends inside quoted string");
			return NULL;
		}
		if (*p == '\r' || *p == '\n')
		{
			config_file_error(cf, "Newline inside quoted string");
			return NULL;
		}
		if (*p != '"')
		{
			config_file_error(cf, "Weird character terminating quoted string (BUG)");
			return NULL;
		}
		p++;
		*q = '\0';
		*pos = p;
		skip_ws(pos, cf);
		return start;
	}
	else
	{
		start = p;
		while (*p != '\0' && *p != '\t' && *p != '\r' && *p != '\n' &&
				*p != ' ' && *p != '/' && *p != '#' &&
				*p != ';' && *p != '{' && *p != '}')
			p++;
		if (p == start)
			return NULL;
		*pos = p;
		skip_ws(pos, cf);
		if (p == *pos)
			*skipped = *p;
		*p = '\0';
		if (p == *pos)
			(*pos)++;
		return start;
	}
}

static config_file_t *config_file_parse(const char *filename, char *confdata)
{
	config_file_t *cf, *subcf, *lastcf;
	config_file_entry_t **pprevce, *ce, *upce;
	char *p, *val;
	char c;

	cf = alloc(sizeof *cf);
	memset(cf,0,sizeof *cf);
	cf->filename = strdup(filename);
	cf->curline = 1;
	cf->mem = confdata;
	lastcf = cf;
	pprevce = &cf->entries;
	upce = NULL;
	p = confdata;
	while (*p != '\0')
	{
		skip_ws(&p, cf);
		if (*p == '\0' || CF_ERRORED(cf))
			break;
		if (*p == '}')
		{
			if (upce == NULL)
			{
				config_file_error(cf, "Extraneous closing brace");
				break;
			}
			ce = upce;
			ce->sectlinenum = cf->curline;
			pprevce = &ce->next;
			upce = ce->prevlevel;
			p++;
			skip_ws(&p, cf);
			if (CF_ERRORED(cf))
				break;
			if (*p != ';')
			{
				config_file_error(cf, "Missing semicolon after closing brace for section ending at line %d", ce->sectlinenum);
				break;
			}
			ce = NULL;
			p++;
			continue;
		}
		val = get_value(&p, cf, &c);
		if (CF_ERRORED(cf))
			break;
		if (val == NULL)
		{
			config_file_error(cf, "Unexpected character trying to read variable name");
			break;
		}
		ce = alloc(sizeof *ce);
		memset(ce, 0, sizeof *ce);
		ce->fileptr = cf;
		ce->varlinenum = cf->curline;
		ce->varname = val;
		ce->prevlevel = upce;
		*pprevce = ce;
		pprevce = &ce->next;
		if (c == '\0' && (*p == '{' || *p == ';'))
			c = *p++;
		if (c == '{')
		{
			pprevce = &ce->entries;
			upce = ce;
			ce = NULL;
		}
		else if (c == ';')
		{
			ce = NULL;
		}
		else if (c != '\0')
		{
			config_file_error(cf, "Unexpected characters after unquoted string %s", ce->varname);
			break;
		}
		else
		{
			val = get_value(&p, cf, &c);
			if (CF_ERRORED(cf))
				break;
			if (val == NULL)
			{
				config_file_error(cf, "Unexpected character trying to read value for %s", ce->varname);
				break;
			}
			ce->vardata = val;
			if (c == '\0' && (*p == '{' || *p == ';'))
				c = *p++;
			if (c == '{')
			{
				pprevce = &ce->entries;
				upce = ce;
				ce = NULL;
			}
			else if (c == ';')
			{
				if (upce == NULL && !strcasecmp(ce->varname, "include"))
				{
					subcf = config_file_load_internal(cf, ce->vardata);
					if (subcf == NULL)
					{
						config_file_error(cf, "Error in file included from here");
						break;
					}
					lastcf->next = subcf;
					while (lastcf->next != NULL)
						lastcf = lastcf->next;
				}
				ce = NULL;
			}
			else
			{
				config_file_error(cf, "Unexpected characters after value %s %s", ce->varname, ce->vardata);
				break;
			}
		}
	}
	if (!CF_ERRORED(cf) && upce != NULL)
	{
		config_file_error(cf, "One or more sections not closed");
		ce = upce;
		while (ce->prevlevel != NULL)
			ce = ce->prevlevel;
		if (ce->vardata != NULL)
			config_file_error(cf, "First unclosed section is %s %s at line %d",
					ce->varname, ce->vardata, ce->varlinenum);
		else
			config_file_error(cf, "First unclosed section is %s at line %d",
					ce->varname, ce->varlinenum);
	}
	if (CF_ERRORED(cf))
	{
		config_file_free(cf);
		cf = NULL;
	}
	return cf;
}

static void config_file_entry_free(config_file_entry_t *ceptr)
{
	config_file_entry_t *nptr;

	for (; ceptr; ceptr = nptr)
	{
		nptr = ceptr->next;
		if (ceptr->entries) {
			config_file_entry_free(ceptr->entries);
			ceptr->entries = NULL;
		}
		/* ce_varname and ce_vardata are inside cf_mem */
		free(ceptr);
		ceptr = NULL;
	}
}

void config_file_free(config_file_t *cfptr)
{
	config_file_t *nptr;

	for (; cfptr; cfptr = nptr)
	{
		nptr = cfptr->next;
		if (cfptr->entries) {
			config_file_entry_free(cfptr->entries);
			cfptr->entries = NULL;
		}
		free(cfptr->filename);
		free(cfptr->mem);
		free(cfptr);
		cfptr->filename = NULL;
		cfptr->mem = NULL;
		cfptr = NULL;
	}
}

static config_file_t *config_file_load_internal(config_file_t *parent, const char *filename)
{
	struct stat sb;
	FILE *fp;
	size_t ret;
	char *buf = NULL;
	config_file_t *cfptr;
	static int nestcnt;

	if (nestcnt > MAX_INCLUDE_NESTING)
	{
		config_file_error(parent, "Includes nested too deep \"%s\"\n", filename);
		return NULL;
	}

	fp = fopen(filename, "rb");
	if (!fp)
	{
		config_file_error(parent, "Couldn't open \"%s\": %s\n", filename, strerror(errno));
		return NULL;
	}
	if (stat(filename, &sb) == -1)
	{
		config_file_error(parent, "Couldn't fstat \"%s\": %s\n", filename, strerror(errno));
		fclose(fp);
		return NULL;
	}
	if (!S_ISREG(sb.st_mode))
	{
		config_file_error(parent, "Not a regular file: \"%s\"\n", filename);
		fclose(fp);
		return NULL;
	}
	if (sb.st_size > SSIZE_MAX - 1)
	{
		config_file_error(parent, "File too large: \"%s\"\n", filename);
		fclose(fp);
		return NULL;
	}
	buf = (char *) alloc(sb.st_size + 1);
	memset(buf, 0, sb.st_size+1);
	if (sb.st_size)
	{
		errno = 0;
		ret = fread(buf, 1, sb.st_size, fp);
		if (ret != (size_t)sb.st_size)
		{
			config_file_error(parent, "Error reading \"%s\": %s\n", filename, strerror(errno ? errno : EFAULT));
			free(buf);
			fclose(fp);
			return NULL;
		}
	}
	else
		ret = 0;
	buf[ret] = '\0';
	fclose(fp);
	nestcnt++;
	cfptr = config_file_parse(filename, buf);
	nestcnt--;
	/* buf is owned by cfptr or freed now */
	return cfptr;
}

config_file_t *config_file_load(const char *filename)
{
	return config_file_load_internal(NULL, filename);
}

/* vim:cinoptions=>s,e0,n0,f0,{0,}0,^0,=s,ps,t0,c3,+s,(2s,us,)20,*30,gs,hs
 * vim:ts=8
 * vim:sw=8
 * vim:noexpandtab
 */
