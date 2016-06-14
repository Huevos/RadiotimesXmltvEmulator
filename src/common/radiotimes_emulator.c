#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <malloc.h>

#ifdef E1
#include <ost/dmx.h>
#define dmx_pes_filter_params dmxPesFilterParams
#define dmx_sct_filter_params dmxSctFilterParams
#else
#include <linux/dvb/dmx.h>
#endif

#include "../common.h"

#include "core/log.h"
#include "dvb/dvb.h"

#include "opentv/opentv.h"
#include "opentv/huffman.h"
#include "providers/providers.h"

#include "epgdb/epgdb.h"
#include "epgdb/epgdb_channels.h"
#include "epgdb/epgdb_titles.h"

buffer_t buffer[65536];
unsigned short buffer_index;
unsigned int buffer_size;
unsigned int buffer_size_last;
bool huffman_debug_titles = false;
bool huffman_debug_summaries = false;

char *db_root = DEFAULT_DB_ROOT;
char demuxer[256];
char provider[256];
char homedir[256];
int frontend = 0;

static volatile bool stop = false;
static volatile bool exec = false;
static volatile bool quit = false;
static volatile bool timeout_enable = true;
pthread_mutex_t mutex;
int timeout = 0;

bool iactive = false;

bool bat_callback (int size, unsigned char* data)
{
	if (data[0] == 0x4a) opentv_read_channels_bat (data, size, db_root);
		if (iactive) log_add ("Reading.. %d channels", opentv_channels_count ());
	return !stop;
}

static void format_size (char *string, int size)
{
	if (size > (1024*1024))
	{
		int sz = size / (1024*1024);
		int dc = (size % (1024*1024)) / (1024*10);
		if (dc > 0)
		{
			if (dc < 10)
				sprintf (string, "%d.0%d MB", sz, dc);
			else if (dc < 100)
				sprintf (string, "%d.%d MB", sz, dc);
			else
				sprintf (string, "%d.99 MB", sz);
		}
		else
			sprintf (string, "%d MB", sz);
	}
	else if (size > 1024)
		sprintf (string, "%d KB", (size / 1024));
	else
		sprintf (string, "%d bytes", size);
}

char *replace_str(char *str, char *orig, char *rep)
{
  static char buffer[4096];
  char *p;

  if(!(p = strstr(str, orig)))  // Is 'orig' even in 'str'?
    return str;

  strncpy(buffer, str, p-str); // Copy characters from 'str' start to 'orig' st$
  buffer[p-str] = '\0';

  sprintf(buffer+(p-str), "%s%s", rep, p+strlen(orig));

  return buffer;
}

bool opentv_titles_callback (int size, unsigned char* data)
{
	char fsize[256];
	if ((data[0] != 0xa0) && (data[0] != 0xa1) && (data[0] != 0xa2) && (data[0] != 0xa3)) return !stop;
	buffer[buffer_index].size = size;
	buffer[buffer_index].data = _malloc (size);
	memcpy(buffer[buffer_index].data, data, size);
	buffer_index++;
	buffer_size += size;
	if (buffer_size_last + 100000 < buffer_size)
	{
		format_size (fsize, buffer_size);
		if (iactive) log_add ("Reading.. %s", fsize);
		buffer_size_last = buffer_size;
	}
	return !stop;
}

bool opentv_summaries_callback (int size, unsigned char* data)
{
	char fsize[256];
	buffer[buffer_index].size = size;
	buffer[buffer_index].data = _malloc (size);
	memcpy(buffer[buffer_index].data, data, size);
	buffer_index++;
	buffer_size += size;
	if (buffer_size_last + 100000 < buffer_size)
	{
		format_size (fsize, buffer_size);
		if (iactive) log_add ("Reading.. %s", fsize);
		buffer_size_last = buffer_size;
	}
	return !stop;
}

void download_opentv ()
{
	int i;
	dvb_t settings;
	char dictionary[256];
	char themes[256];

	log_add ("Started RadioTimes XMLTV emulation");
	log_add ("Started OpenTV events download");

	sprintf (dictionary, "%s/providers/%s.dict", homedir, provider);
	sprintf (themes, "%s/providers/%s.themes", homedir, provider);

	opentv_init ();
	if (huffman_read_dictionary (dictionary) && opentv_read_themes (themes))
	{
		char size[256];
	
		settings.pids = providers_get_channels_pids ();
		settings.pids_count = providers_get_channels_pids_count ();
		settings.demuxer = demuxer;
		settings.frontend = frontend;
		settings.min_length = 11;
		settings.buffer_size = 4 * 1024;
		settings.filter = 0x4a;
		settings.mask = 0xff;
	
		log_add ("Reading channels...");
		dvb_read (&settings, *bat_callback);
		print_meminfo ();
		log_add ("Read %d channels", opentv_channels_count ());
		if (stop) goto opentv_stop;
	
		settings.pids = providers_get_titles_pids ();
		settings.pids_count = providers_get_titles_pids_count ();
		settings.demuxer = demuxer;
		settings.frontend = frontend;
		settings.min_length = 20;
		settings.buffer_size = 4 * 1024;
		settings.filter = 0xa0;
		settings.mask = 0xfc;
	
		buffer_index = 0;
		buffer_size = 0;
		buffer_size_last = 0;
		log_add ("Reading titles...");
		dvb_read (&settings, *opentv_titles_callback);
		print_meminfo ();
		format_size (size, buffer_size);
		log_add ("Read %s", size);
		if (stop) goto opentv_stop;
	
		log_add ("Parsing titles...");
		buffer_size = 0;
		time_t lasttime = 0;
		for (i=0; i<buffer_index; i++)
		{
			if (!stop) opentv_read_titles (buffer[i].data, buffer[i].size, huffman_debug_titles);
			buffer_size += buffer[i].size;
			_free (buffer[i].data);
			if ((i % 100) == 0)
			{
				if (lasttime != time (NULL) || (i == buffer_index-1))
				{
					lasttime = time (NULL);
					format_size (size, buffer_size);
					if (iactive)
					{
						log_add ("Parsing.. %s", size);
						log_add ("Progress %%%d", (i*100)/buffer_index);
					}
					print_meminfo ();
				}
			}
		}
		format_size (size, buffer_size);
		log_add ("Parsed %s", size);
		log_add ("Titles parsed");
		if (stop) goto opentv_stop;
	
		settings.pids = providers_get_summaries_pids ();
		settings.pids_count = providers_get_summaries_pids_count ();
		settings.demuxer = demuxer;
		settings.frontend = frontend;
		settings.min_length = 20;
		settings.buffer_size = 4 * 1024;
		settings.filter = 0xa8;
		settings.mask = 0xfc;
	
		buffer_index = 0;
		buffer_size = 0;
		buffer_size_last = 0;
		log_add ("Reading summaries...");
		dvb_read (&settings, *opentv_summaries_callback);
		print_meminfo ();
		format_size (size, buffer_size);
		log_add ("Read %s", size);
		if (stop) goto opentv_stop;
	
		log_add ("Parsing summaries...");
		buffer_size = 0;
		lasttime = 0;
		for (i=0; i<buffer_index; i++)
		{
			if (!stop) opentv_read_summaries (buffer[i].data, buffer[i].size, huffman_debug_summaries, db_root);
			buffer_size += buffer[i].size;
			_free (buffer[i].data);
			if ((i % 100) == 0)
			{
				if (lasttime != time (NULL) || (i == buffer_index-1))
				{
					lasttime = time (NULL);
					format_size (size, buffer_size);
					if (iactive)
					{
						log_add ("Parsing.. %s", size);
						log_add ("Progress %%%d", (i*100)/buffer_index);
					}
					print_meminfo ();
				}
			}
		}
		format_size (size, buffer_size);
		log_add ("Parsed %s", size);
		log_add ("Summaries parsed");
opentv_stop:
		huffman_free_dictionary ();
		epgdb_clean ();
		opentv_cleanup();
	}

	exec = false;
	log_add ("Ended RadioTimes XMLTV emulation");
}

void *download (void *args)
{
	int i;
	char opentv_file[256];

	sprintf (opentv_file, "%s/providers/%s.conf", homedir, provider);

	if (providers_read (opentv_file))
	{
		if (providers_get_protocol () == 1)
		{
			download_opentv ();
		}
		else
		{
			log_add ("invalid provider");
			exec = false;
		}
	}
	else
	{
		log_add ("cannot read provider");
		exec = false;
	}

	return NULL;
}

int main (int argc, char **argv)
{
	int c, i;
	opterr = 0;

	strcpy (homedir, argv[0]);
	for (i = strlen (homedir)-1; i >= 0; i--)
	{
		bool ended = false;
		if (homedir[i] == '/') ended = true;
		homedir[i] = '\0';
		if (ended) break;
	}

	strcpy (demuxer, DEFAULT_DEMUXER);
	strcpy (provider, DEFAULT_OTV_PROVIDER);

	while ((c = getopt (argc, argv, "h:d:x:f:l:p:k:ryz")) != -1)
	{
		switch (c)
		{
			case 'd':
				db_root = optarg;
				break;
			case 'x':
				strcpy (demuxer, optarg);
				break;
			case 'f':
				frontend = atoi(optarg);
				break;
			case 'l':
				strcpy (homedir, optarg);
				break;
			case 'p':
				strcpy (provider, optarg);
				break;
			case 'k':
				nice (atoi(optarg));
				break;
			case 'r':
				//log_disable ();
				iactive = true;
				break;
			case 'y':
				huffman_debug_summaries = true;
				break;
			case 'z':
				huffman_debug_titles = true;
				break;
			case '?':
				printf ("Usage:\n");
				printf ("  ./radiotimes_emulator [options]\n");
				printf ("Options:\n");
				printf ("  -d db_root    radiotimes db root folder\n");
				printf ("                default: %s\n", db_root);
				printf ("  -x demuxer    dvb demuxer\n");
				printf ("                default: %s\n", demuxer);
				printf ("  -f frontend   dvb frontend\n");
				printf ("                default: %d\n", frontend);
				printf ("  -l homedir    home directory\n");
				printf ("                default: %s\n", homedir);
				printf ("  -p provider   opentv provider\n");
				printf ("                default: %s\n", provider);
				printf ("  -k nice       see \"man nice\"\n");
				printf ("  -r            show progress\n");
				printf ("  -y            debug mode for huffman dictionary (summaries)\n");
				printf ("  -z            debug mode for huffman dictionary (titles)\n");
				printf ("  -h            show this help\n");
				return 0;
		}
	}
	
	while (homedir[strlen (homedir) - 1] == '/') homedir[strlen (homedir) - 1] = '\0';
	while (db_root[strlen (db_root) - 1] == '/') db_root[strlen (db_root) - 1] = '\0';
	
	mkdir (db_root, S_IRWXU|S_IRWXG|S_IRWXO);
	
	log_open (db_root);
	log_banner ("RadioTimes XMLTV Emulator");


	char opentv_file[256];

	sprintf (opentv_file, "%s/providers/%s.conf", homedir, provider);
	if (providers_read (opentv_file))
	{
		if (providers_get_protocol () == 1)
		{
			log_add ("Provider %s identified as opentv", provider);
			download_opentv ();
			epgdb_clean ();
			print_meminfo ();
		}
	}
	else
		log_add ("Cannot load provider configuration (%s)", opentv_file);
	
	memory_stats ();
error:
	log_close ();
	return 0;
}
