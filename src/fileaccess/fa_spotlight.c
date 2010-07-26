/*
 *  Spotlight search backend
 *  Copyright (C) 2010 Mattias Wadman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <CoreServices/CoreServices.h>

#include "showtime.h"
#include "backend/backend.h"
#include "fileaccess.h"
#include "fa_probe.h"
#include "fa_search.h"
#include "service.h"
#include "settings.h"


static int spotlight_enabled;

typedef struct fa_search_s {
  char                 *fas_query;
  backend_search_type_t fas_type;
  prop_t               *fas_nodes;
  prop_sub_t           *fas_sub;
  prop_courier_t       *fas_pc;
  int                   fas_run;
} fa_search_t;

static void
fa_search_destroy(fa_search_t *fas)
{
  free(fas->fas_query);
  
  if (fas->fas_pc)
    prop_courier_destroy(fas->fas_pc);
  
  if (fas->fas_sub)
    prop_unsubscribe(fas->fas_sub);
  
  if (fas->fas_nodes)
    prop_ref_dec(fas->fas_nodes);
  
  free(fas);
}

static CFArrayRef
spotlight_scope_directories(void) {
  CFMutableArrayRef directories;
  service_t *s;
  
  directories = CFArrayCreateMutable(kCFAllocatorDefault,
                                     0,
                                     &kCFTypeArrayCallBacks);
  
  hts_mutex_lock(&service_mutex);
  
  LIST_FOREACH(s, &services, s_link) {
    if(strncmp(s->s_url, "file://", strlen("file://")) != 0)
      continue;
    
    CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault,
                                                 s->s_url + strlen("file://"),
                                                 kCFStringEncodingUTF8);
    CFArrayAppendValue(directories, path);
    CFRelease(path);
  }
  
  hts_mutex_unlock(&service_mutex);
  
  return directories;
}

static void
spotlight_search_nodesub(void *opaque, prop_event_t event, ...)
{
  fa_search_t *fas = opaque;
  va_list ap;
  event_t *e;
  
  va_start(ap, event);
  
  switch(event)
  {
    case PROP_DESTROYED:
      fas->fas_run = 0;
      break;
      
    case PROP_EXT_EVENT:
      e = va_arg(ap, event_t *);
      
      if(e->e_type_x != EVENT_APPEND_REQUEST)
	break;
      
      /* FIXME: Support offsets. */
      break;
      
    default:
      break;
  }
  
  va_end(ap);
}

static void *
spotlight_searcher(void *aux)
{
  fa_search_t *fas = aux;
  CFArrayRef directories;
  MDQueryRef query;
  CFMutableStringRef query_string;
  CFIndex query_count;
  CFIndex query_index;
  
  fas->fas_pc = prop_courier_create_passive();
  fas->fas_sub = 
  prop_subscribe(PROP_SUB_TRACK_DESTROY,
                 PROP_TAG_CALLBACK, spotlight_search_nodesub, fas,
                 PROP_TAG_ROOT, fas->fas_nodes,
                 PROP_TAG_COURIER, fas->fas_pc,
                 NULL);
  
  query_string = CFStringCreateMutable(kCFAllocatorDefault, 0);
  CFStringAppendCString(query_string, "kMDItemFSName = '*",
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, fas->fas_query,
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, "*'cd || ",
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, "kMDItemAlbum = '*",
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, fas->fas_query,
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, "*'cd || ",
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, "kMDItemAuthors = '*",
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, fas->fas_query,
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, "*'cd || ",
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, "kMDItemTitle = '*",
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, fas->fas_query,
                        kCFStringEncodingUTF8);
  CFStringAppendCString(query_string, "*'cd",
                        kCFStringEncodingUTF8);
  query = MDQueryCreate(kCFAllocatorDefault,
                        query_string,
                        NULL,
                        NULL);
  CFRelease(query_string);
  directories = spotlight_scope_directories();
  MDQuerySetSearchScope(query, directories, 0);
  CFRelease(directories);
  MDQueryExecute(query, kMDQuerySynchronous);
  MDQueryStop(query);
  query_count = MDQueryGetResultCount(query);
  query_index = 0;
  
  while (1) {
    prop_t *p, *metadata;
    const char *type;
    int ctype;
    MDItemRef item;
    CFStringRef pathRef;
    int len;
    char *path;
    int match = 1;

    prop_courier_poll(fas->fas_pc);
    
    if(!fas->fas_run)
      break;
    
    if(query_index >= query_count)
      break;
    
    item = (MDItemRef)MDQueryGetResultAtIndex(query, query_index);
    query_index++;
    
    metadata = prop_create(NULL, "metadata");
    
    pathRef = (CFStringRef)MDItemCopyAttribute(item, kMDItemPath);
    
    len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(pathRef),
                                            kCFStringEncodingUTF8) + 1;
    path = malloc(len);
    CFStringGetCString(pathRef, path, len, kCFStringEncodingUTF8);
    CFRelease(pathRef);
    ctype = fa_probe(metadata, path, NULL, 0, NULL, 0, NULL);
    
    /* Filter out based on search type. */
    switch (fas->fas_type)
    {
	/* FIXME: We dont want to include matching dirs whose content
	 *        doesn't match the fas_type. */
      case BACKEND_SEARCH_AUDIO:
	if(ctype != CONTENT_AUDIO)
          match = 0;
	break;
	
      case BACKEND_SEARCH_VIDEO:
	if(ctype != CONTENT_VIDEO &&
           ctype != CONTENT_DVD)
          match = 0;
	break;
        
      case BACKEND_SEARCH_ALL:
	break;
    }
    
    if(ctype == CONTENT_UNKNOWN)
      match = 0;
    
    if(match) {
      p = prop_create(NULL, NULL);
      
      if (prop_set_parent(metadata, p))
        prop_destroy(metadata);
      
      prop_set_string(prop_create(p, "url"), path);
      type = content2type(ctype);
      if(type != NULL)
        prop_set_string(prop_create(p, "type"), type);
      
      if (prop_set_parent(p, fas->fas_nodes))
        prop_destroy(p);
    }
    
    free(path);
  }
  
  CFRelease(query);
  
  TRACE(TRACE_DEBUG, "FA", "Searcher: %s: Done", fas->fas_query);
  fa_search_destroy(fas);
  
  return NULL;
}

static void
spotlight_search(struct backend *be, prop_t *source,
		 const char *query, backend_search_type_t type)
{
  if(!spotlight_enabled)
    return;
  
  fa_search_t *fas = calloc(1, sizeof(*fas));
  char *s;
  
  fas->fas_query = s = strdup(query);
  fas->fas_run = 1;
  fas->fas_type = type;
  fas->fas_nodes = prop_create(source, "nodes");
  prop_ref_inc(fas->fas_nodes);
  
  hts_thread_create_detached("spotlight search", spotlight_searcher, fas);
}

static int
spotlight_init(void)
{
  htsmsg_t *store = htsmsg_store_load("spotlight") ?: htsmsg_create_map();
  prop_t *s = search_get_settings();
  
  settings_create_bool(s, "enable", "Search using spotlight", 1, 
		       store, settings_generic_set_bool, &spotlight_enabled,
		       SETTINGS_INITIAL_UPDATE, NULL,
		       settings_generic_save_settings, (void *)"spotlight");
  return 0;
}

backend_t be_spotlight = {
  .be_init = spotlight_init,
  .be_search = spotlight_search
};

BE_REGISTER(spotlight);