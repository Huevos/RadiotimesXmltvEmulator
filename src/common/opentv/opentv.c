#include <stdio.h>
#include <time.h>
#include <memory.h>
#include <malloc.h>
#include <stdint.h>

#include "../../common.h"
#include "../core/log.h"

#include "huffman.h"
#include "opentv.h"

#include "../epgdb/epgdb_channels.h"
#include "../epgdb/epgdb_titles.h"

#define MAX_TITLE_SIZE		256
#define MAX_SUMMARIE_SIZE	16384
#define MAX_CHANNELS		65536

static epgdb_channel_t *channels[MAX_CHANNELS];

static unsigned short int ch_count;
static int tit_count;

void removeSubstring(char *s,const char *toremove)
{
  while( (s=strstr(s,toremove)) )
    memmove(s,s+strlen(toremove),1+strlen(s+strlen(toremove)));
}

void opentv_init ()
{
	int i;
	ch_count = 0;
	tit_count = 0;
	for (i=0; i<MAX_CHANNELS; i++)
		channels[i] = NULL;
	for (i=0; i<256; i++)
		genre[i] = NULL;
}

void opentv_cleanup ()
{
	int i;
	for (i=0; i<256; i++)
	{
		if (genre[i] != NULL)
		{
			_free (genre[i]);
		}
	}
}

bool opentv_read_channels_bat (unsigned char *data, unsigned int length, char *db_root)
{
	unsigned short int bouquet_descriptors_length = ((data[8] & 0x0f) << 8) | data[9];
	unsigned short int transport_stream_loop_length = ((data[bouquet_descriptors_length + 10] & 0x0f) << 8) | data[bouquet_descriptors_length + 11];
	unsigned int offset1 = bouquet_descriptors_length + 12;
	bool ret = false;
	
	while (transport_stream_loop_length > 0)
	{
		unsigned short int tid = (data[offset1] << 8) | data[offset1 + 1];
		unsigned short int nid = (data[offset1 + 2] << 8) | data[offset1 + 3];
		unsigned short int transport_descriptor_length = ((data[offset1 + 4] & 0x0f) << 8) | data[offset1 + 5];
		unsigned int offset2 = offset1 + 6;
		
		offset1 += (transport_descriptor_length + 6);
		transport_stream_loop_length -= (transport_descriptor_length + 6);
		
		while (transport_descriptor_length > 0)
		{
			unsigned char descriptor_tag = data[offset2];
			unsigned char descriptor_length = data[offset2 + 1];
			unsigned int offset3 = offset2 + 2;
			
			offset2 += (descriptor_length + 2);
			transport_descriptor_length -= (descriptor_length + 2);
			
			if (descriptor_tag == 0xb1)
			{
				offset3 += 2;
				descriptor_length -= 2;
				while (descriptor_length > 0)
				{
					unsigned short int type_id;
					unsigned short int channel_id;
					unsigned short int sid;
					//unsigned short int sky_id;

					sid = (data[offset3] << 8) | data[offset3 + 1];
					type_id = data[offset3 + 2];
					channel_id = (data[offset3 + 3] << 8) | data[offset3 + 4];
					//sky_id = ( data[offset3+5] << 8 ) | data[offset3+6];

					if (channels[channel_id] == NULL)
					{
						FILE *outfile;
						char name_file[256];
						memset(name_file, '\0', 256);
						sprintf(name_file, "%s/channels.dat", db_root);
						outfile = fopen(name_file,"a");

						fprintf(outfile,"%i|%x:%x:%x\n",
							channel_id,
							nid, tid, sid);
						fflush(outfile);
						fclose(outfile);

						channels[channel_id] = epgdb_channels_add (nid, tid, sid, type_id);
						ch_count++;
						ret = true;
					}
					
					offset3 += 9;
					descriptor_length -= 9;
				}
			}
		}
	}
	return ret;
}

unsigned short opentv_channels_count()
{
	return ch_count;
}

void opentv_read_titles (unsigned char *data, unsigned int length, bool huffman_debug)
{
	epgdb_title_t *title;
	unsigned short int channel_id = (data[3] << 8) | data[4];
	unsigned short int mjd_time = (data[8] << 8) | data[9];
	
	if ((channel_id > 0) && (mjd_time > 0))
	{
		unsigned int offset = 10;
		
		while ((offset + 11) < length)
		{
			unsigned short int event_id;
			unsigned char description_length;
			unsigned short int packet_length = ((data[offset + 2] & 0x0f) << 8) | data[offset + 3];
			
			if ((data[offset + 4] != 0xb5) || ((packet_length + offset) > length)) break;
			
			event_id = (data[offset] << 8) | data[offset + 1];
			offset += 4;
			description_length = data[offset + 1] - 7;
			
			if ((offset + 9 + description_length) > length) break;
			
			if (channels[channel_id] != NULL)
			{
				char tmp[256];
				
				/* prepare struct */
				title = _malloc (sizeof (epgdb_title_t));
				title->event_id = event_id;
				title->start_time = ((mjd_time - 40587) * 86400) + ((data[offset + 2] << 9) | (data[offset + 3] << 1));
				title->mjd = mjd_time;
				title->length = ((data[offset + 4] << 9) | (data[offset + 5] << 1));
				title->genre_id = data[offset + 6];
				
				if (!huffman_decode (data + offset + 9, description_length, tmp, MAX_TITLE_SIZE * 2, huffman_debug))
					tmp[0] = '\0';
				else
					tmp[35] = '\0';

				strcpy(title->program, tmp);
				title = epgdb_titles_add (channels[channel_id], title);

				if (huffman_debug)
				{
					char mtime[20];
					struct tm *loctime = localtime ((time_t*)&title->start_time);
					printf ("Nid: %x Tsid: %x Sid: %x Type: %x\n", channels[channel_id]->nid, channels[channel_id]->tsid, channels[channel_id]->sid, channels[channel_id]->type);
					strftime (mtime, 20, "%d/%m/%Y %H:%M", loctime);
					printf ("Start time: %s\n", mtime);
				}
					tit_count++;
			}

			offset += packet_length;
		}
	}
}

void opentv_read_summaries (unsigned char *data, unsigned int length, bool huffman_debug, char *db_root)
{
	if (length < 20) return;

	unsigned short int channel_id = (data[3] << 8) | data[4];
	unsigned short int mjd_time = (data[8] << 8) | data[9];
	
	if ((channel_id > 0) && (mjd_time > 0))
	{
		unsigned int offset = 10;

		while (offset + 4 < length)
		{
			unsigned short int event_id;
			int packet_length = ((data[offset + 2] & 0x0f) << 8) | data[offset + 3];
			int packet_length2 = packet_length;
			unsigned char buffer[MAX_SUMMARIE_SIZE];
			unsigned short int buffer_size = 0;
			unsigned int offset2;

			if (packet_length == 0) break;

			event_id = (data[offset] << 8) | data[offset + 1];
			offset += 4;
			offset2 = offset;
			while (packet_length2 > 0)
			{
				unsigned char descriptor_tag = data[offset2];
				unsigned char descriptor_length = data[offset2 + 1];

				offset2 += 2;

				if (descriptor_tag == 0xb9 &&
					MAX_SUMMARIE_SIZE > buffer_size + descriptor_length &&
					offset2 + descriptor_length < length)
				{
					memcpy(&buffer[buffer_size], &data[offset2], descriptor_length);
					buffer_size += descriptor_length;
				}

				packet_length2 -= descriptor_length + 2;
				offset2 += descriptor_length;
			}

			offset += packet_length;

			if (buffer_size > 0 && channels[channel_id] != NULL)
			{
				epgdb_title_t *title = epgdb_titles_get_by_id_and_mjd (channels[channel_id], event_id, mjd_time);
				if (title != NULL)
				{
					char tmp[MAX_SUMMARIE_SIZE * 2];
					if (!huffman_decode (buffer, buffer_size, tmp, MAX_SUMMARIE_SIZE * 2, huffman_debug))
						tmp[0] = '\0';
					else
						removeSubstring(tmp," Also in HD");

					if (huffman_debug)
					{
						char mtime[20];
						struct tm *loctime = localtime ((time_t*)&title->start_time);
						printf ("Nid: %x Tsid: %x Sid: %x Type: %x\n", channels[channel_id]->nid, channels[channel_id]->tsid, channels[channel_id]->sid, channels[channel_id]->type);
						strftime (mtime, 20, "%d/%m/%Y %H:%M", loctime);
						printf ("Start time: %s\n", mtime);
						
					}
/*
	RadioTimes XMLTV feed Structure
	###############################
	# 01 Programme Title
	# 02 Sub-Title
	# 03 Episode
	# 04 Year
	# 05 Director
	# 06 Performers (Cast) - This will be either a string containing the Actors names or be made up of 
	#    Character name and Actor name pairs which are separated by an asterix "*" and each pair by pipe "|" 
	#    e.g. Rocky*Sylvester Stallone|Terminator*Arnold Schwarzenegger. 
	# 07 Premiere
	# 08 Film
	# 09 Repear
	# 10 Subtitles
	# 11 Widescreen
	# 12 New series
	# 13 Deaf signed
	# 14 Black and White
	# 15 Film star rating
	# 16 Film certificate
	# 17 Genre
	# 18 Description
	# 19 Radio Times Choice - This means that the Radio Times editorial team have marked it as a choice
	# 20 Date
	# 21 Start Time
	# 22 End Time
	# 23 Duration (Minutes)

	Close~~~~~~false~false~false~false~false~false~false~false~~~No Genre~~false~19/05/2011~02:30~10:00~450
	The Bounty Hunter~~~2010~Andy Tennant~Milo Boyd*Gerard Butler|Nicole Hurley*Jennifer Aniston~false~true~false~true~true~false~false~false~2~12~Film~Cop-turned-bounty hunter is assigned to track down his bail-jumping reporter ex-wife. They are in turn pursued by crooked detectives who fear the feisty Nicole is getting too close to the truth.~false~19/05/2011~10:00~12:00~120
	Family Show~~~~~~false~false~false~false~true~false~false~false~~~Entertainment~The latest films from the UK and the US.~false~19/05/2011~12:00~12:30~30
*/
					char mtime_s[20];
					struct tm *loctime_s = localtime ((time_t*)&title->start_time);
					strftime (mtime_s, 20, "%d/%m/%Y~%H:%M", loctime_s);

					char mtime_e[20];
					uint32_t endt;
					endt = (title->start_time + title->length);
					struct tm *loctime_e = localtime ((time_t*)&endt);
					strftime (mtime_e, 10, "%H:%M", loctime_e);

					FILE *outfile;
					char name_file[256];
					memset(name_file, '\0', 256);
					sprintf(name_file, "%s/%i.dat", db_root, channel_id);
					outfile = fopen(name_file,"a");

					fprintf(outfile,"%s~~~~~~~~~~~~~~~~%s~%s~~%s~%s~%i\n",
						title->program,
						genre[title->genre_id],
						tmp,
						mtime_s,
						mtime_e,
						(title->length / 60));
					fflush(outfile);
					fclose(outfile);

					epgdb_titles_delete_event_id(channels[channel_id], title->event_id);
				}
			}
		}
	}
}

bool opentv_read_themes (char *file)
{
	FILE *fd;
	char line[256];

	log_add ("Reading themes '%s'", file);

	fd = fopen (file, "r");
	if (!fd) 
	{
		log_add ("Error. Cannot open themes file");
		return false;
	}

	int genre_id = 0;
	char string1[256];
	char string2[256];

	while (fgets (line, sizeof(line), fd))
	{
		memset(string1, 0, sizeof(string1));
		memset(string2, 0, sizeof(string2));

		if(sscanf(line, "%[^=] =%[^\n] ", string1, string2) == 2)
		{
			genre[genre_id] = _malloc (sizeof (char) * (strlen (string2) + 1));
			snprintf((char *) genre[genre_id], 255, "%s", string2);
		}
		else
		{
			genre[genre_id] = _malloc (sizeof (char) * (strlen (string1) + 10));
			snprintf((char *) genre[genre_id], 255, "Genre %s", string1);
		}
		genre_id++;
	}
	fclose(fd);

	log_add ("Completed. Read %d values", genre_id);

	return true;
}

epgdb_channel_t *opentv_get_channel (unsigned short int id)
{
	return channels[id];
}
