/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
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
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <stdio.h>
#include <assert.h>

#include "networking/http_server.h"
#include "htsmsg/htsmsg_json.h"
#include "misc/str.h"
#include "prop/prop.h"
#include "misc/redblack.h"
#include "misc/dbl.h"
#include "misc/bytestream.h"
#include "stpp.h"

#include "backend/backend.h"
#include "image/image.h"
#include "fileaccess/fileaccess.h"
#include "task.h"

RB_HEAD(stpp_subscription_tree, stpp_subscription);
RB_HEAD(stpp_prop_tree, stpp_prop);
LIST_HEAD(stpp_prop_list, stpp_prop);
LIST_HEAD(stpp_imagereq_list, stpp_imagereq);

/**
 *
 */
typedef struct stpp {
  http_connection_t *stpp_hc;
  struct stpp_subscription_tree stpp_subscriptions;
  struct stpp_prop_tree stpp_props;
  int stpp_prop_tally;
  int stpp_helloed_ok;
  struct stpp_imagereq_list stpp_imagereqs;
} stpp_t;


/**
 * A subscription as created by the STPP client
 */
typedef struct stpp_subscription {
  RB_ENTRY(stpp_subscription) ss_link;
  unsigned int ss_id;
  prop_sub_t *ss_sub;
  stpp_t *ss_stpp;
  struct stpp_prop_list ss_dir_props;   // Exported props when in dir mode
  struct stpp_prop_list ss_value_props; // Exported props when in value mode
} stpp_subscription_t;

static int
ss_cmp(const stpp_subscription_t *a, const stpp_subscription_t *b)
{
  return a->ss_id - b->ss_id;
}


/**
 * An exported property
 */
typedef struct stpp_prop {
  RB_ENTRY(stpp_prop) sp_link;
  unsigned int sp_id;
  prop_t *sp_prop;
  stpp_subscription_t *sp_sub;
  LIST_ENTRY(stpp_prop) sp_sub_link;
} stpp_prop_t;

static int
sp_cmp(const stpp_prop_t *a, const stpp_prop_t *b)
{
  return a->sp_id - b->sp_id;
}

static stpp_prop_t *
sp_get(prop_t *p, stpp_subscription_t *ss)
{
  return prop_tag_get(p, ss);
}



/**
 *
 */
static stpp_prop_t *
stpp_property_export_from_sub(stpp_subscription_t *ss, prop_t *p,
                              struct stpp_prop_list *list)
{
  stpp_t *stpp = ss->ss_stpp;
  stpp_prop_t *sp = malloc(sizeof(stpp_prop_t));
  sp->sp_id = ++stpp->stpp_prop_tally;
  sp->sp_prop = prop_ref_inc(p);
  sp->sp_sub = ss;
  LIST_INSERT_HEAD(list, sp, sp_sub_link);
  if(RB_INSERT_SORTED(&stpp->stpp_props, sp, sp_link, sp_cmp))
    abort();
  prop_tag_set(p, ss, sp);
  return sp;
}


/**
 *
 */
static void
stpp_property_unexport_from_sub(stpp_subscription_t *ss, stpp_prop_t *sp)
{
  stpp_t *stpp = ss->ss_stpp;
  prop_ref_dec(sp->sp_prop);
  LIST_REMOVE(sp, sp_sub_link);
  RB_REMOVE(&stpp->stpp_props, sp, sp_link);
  free(sp);
}


/**
 *
 */
static prop_t *
resolve_propref(stpp_t *stpp, int propref)
{
  stpp_prop_t skel, *sp;
  if(propref == 0)
    return prop_get_global();
  skel.sp_id = propref;

  if((sp = RB_FIND(&stpp->stpp_props, &skel, sp_link, sp_cmp)) == NULL) {
    TRACE(TRACE_ERROR, "STPP", "Referring unknown propref %d", propref);
    return NULL;
  }
  return sp->sp_prop;
}


/**
 *
 */
static void
stpp_sub_json_add_child(stpp_subscription_t *ss, http_connection_t *hc,
			prop_t *p, prop_t *before)
{ 
  char buf2[128];
  unsigned int b = before ? sp_get(before, ss)->sp_id : 0;
  stpp_prop_t *sp = stpp_property_export_from_sub(ss, p, &ss->ss_dir_props);
  snprintf(buf2, sizeof(buf2), "[5,%u,%u,[%u]]", ss->ss_id, b, sp->sp_id);
  websocket_send(hc, 1, buf2, strlen(buf2));
}


/**
 *
 */
static void
stpp_sub_json_add_childs(stpp_subscription_t *ss, http_connection_t *hc,
			 prop_vec_t *pv, prop_t *before)
{ 
  unsigned int b = before ? sp_get(before, ss)->sp_id : 0;
  int i;
  htsbuf_queue_t hq;

  htsbuf_queue_init(&hq, 0);
  htsbuf_qprintf(&hq, "[5,%u,%u,[", ss->ss_id, b);

  for(i = 0; i < prop_vec_len(pv); i++) {
    prop_t *p = prop_vec_get(pv, i);
    stpp_prop_t *sp = stpp_property_export_from_sub(ss, p, &ss->ss_dir_props);
    htsbuf_qprintf(&hq, "%s%u", i ? "," : "", sp->sp_id);
  }
  htsbuf_append(&hq, "]]", 1);
  websocket_sendq(hc, 1, &hq);
}



/**
 *
 */
static void
stpp_sub_json_del_child(stpp_subscription_t *ss, http_connection_t *hc,
			prop_t *p)
{ 
  stpp_prop_t *sp = prop_tag_clear(p, ss);
  char buf2[128];
  snprintf(buf2, sizeof(buf2), "[6,%u,[%u]]", ss->ss_id, sp->sp_id);
  websocket_send(hc, 1, buf2, strlen(buf2));
  stpp_property_unexport_from_sub(ss, sp);
}



/**
 *
 */
static void
stpp_sub_json_move_child(stpp_subscription_t *ss, http_connection_t *hc,
			 prop_t *p, prop_t *before)
{ 
  stpp_prop_t *sp =          prop_tag_get(p, ss);
  stpp_prop_t *b =  before ? prop_tag_get(before, ss) : NULL;
  char buf2[128];
  snprintf(buf2, sizeof(buf2), "[7,%u,%u,%u]", ss->ss_id, sp->sp_id,
	   b ? b->sp_id : 0);
  websocket_send(hc, 1, buf2, strlen(buf2));
}


/**
 *
 */
static void
ss_clear_props(stpp_subscription_t *ss, struct stpp_prop_list *list)
{
  stpp_prop_t *sp;
  while((sp = LIST_FIRST(list)) != NULL) {
    prop_tag_clear(sp->sp_prop, ss);
    stpp_property_unexport_from_sub(ss, sp);
  }
}


/**
 * Hardwired JSON output
 */
static void
stpp_sub_json(void *opaque, prop_event_t event, ...)
{
  stpp_subscription_t *ss = opaque;
  http_connection_t *hc = ss->ss_stpp->stpp_hc;
  va_list ap;
  htsbuf_queue_t hq;
  char buf[64];
  char buf2[128];
  prop_t *p1;
  prop_vec_t *pv;
  const char *str, *str2;
  va_start(ap, event);
  
  switch(event) {
  case PROP_SET_FLOAT:
    my_double2str(buf, sizeof(buf), va_arg(ap, double));
    snprintf(buf2, sizeof(buf2), "[4,%u,%s]", ss->ss_id, buf);
    websocket_send(hc, 1, buf2, strlen(buf2));
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_INT:
    snprintf(buf2, sizeof(buf2), "[4,%u,%d]", ss->ss_id, va_arg(ap, int));
    websocket_send(hc, 1, buf2, strlen(buf2));
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_RSTRING:
    str = rstr_get(va_arg(ap, rstr_t *));
    if(0)
  case PROP_SET_CSTRING:
      str = va_arg(ap, const char *);

    htsbuf_queue_init(&hq, 0);
    htsbuf_qprintf(&hq, "[4,%u,", ss->ss_id);
    htsbuf_append_and_escape_jsonstr(&hq, str);
    htsbuf_append(&hq, "]", 1);
    websocket_sendq(hc, 1, &hq);
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_VOID:
    snprintf(buf2, sizeof(buf2), "[4,%u,null]", ss->ss_id);
    websocket_send(hc, 1, buf2, strlen(buf2));
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_URI:
    str = rstr_get(va_arg(ap, rstr_t *));
    str2 = rstr_get(va_arg(ap, rstr_t *));
    htsbuf_queue_init(&hq, 0);
    htsbuf_qprintf(&hq, "[4,%u,[\"uri\",", ss->ss_id);
    htsbuf_append_and_escape_jsonstr(&hq, str);
    htsbuf_append(&hq, ",", 1);
    htsbuf_append_and_escape_jsonstr(&hq, str2);
    htsbuf_append(&hq, "]]", 2);
    websocket_sendq(hc, 1, &hq);
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_DIR:
    snprintf(buf2, sizeof(buf2), "[4,%u,[\"dir\"]]", ss->ss_id);
    websocket_send(hc, 1, buf2, strlen(buf2));
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_ADD_CHILD:
    stpp_sub_json_add_child(ss, hc, va_arg(ap, prop_t *), NULL);
    break;
  case PROP_ADD_CHILD_BEFORE:
    p1 = va_arg(ap, prop_t *);
    stpp_sub_json_add_child(ss, hc, p1, va_arg(ap, prop_t *));
    break;

  case PROP_ADD_CHILD_VECTOR:
    stpp_sub_json_add_childs(ss, hc, va_arg(ap, prop_vec_t *), NULL);
    break;

  case PROP_ADD_CHILD_VECTOR_BEFORE:
    pv = va_arg(ap, prop_vec_t *);
    stpp_sub_json_add_childs(ss, hc, pv, va_arg(ap, prop_t *));
    break;

  case PROP_DEL_CHILD:
    stpp_sub_json_del_child(ss, hc, va_arg(ap, prop_t *));
    break;

  case PROP_MOVE_CHILD:
    p1 = va_arg(ap, prop_t *);
    stpp_sub_json_move_child(ss, hc, p1, va_arg(ap, prop_t *));
    break;

  default:
    printf("stpp_sub_json() can't deal with event %d\n", event);
    break;

  }
  va_end(ap);
}

/**
 * Binary output
 */
static void
stpp_sub_binary(void *opaque, prop_event_t event, ...)
{
  stpp_subscription_t *ss = opaque;
  http_connection_t *hc = ss->ss_stpp->stpp_hc;
  va_list ap;
  const char *str;
  uint8_t *buf;
  int buflen = 1 + 1 + 4;
  int len;
  int flags;
  prop_t *p, *before;
  const prop_vec_t *pv;
  stpp_prop_t *sp;
  union {
    float f;
    int i;
  } u;
  va_start(ap, event);

  switch(event) {
  case PROP_SET_INT:
    buflen += 4;
    buf = alloca(buflen);
    buf[1] = STPP_SET_INT;
    wr32_le(buf + 6, va_arg(ap, int));
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_FLOAT:
    buflen += 4;
    buf = alloca(buflen);
    buf[1] = STPP_SET_FLOAT;
    u.f = va_arg(ap, double);
    wr32_le(buf + 6, u.i);
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_VOID:
    buf = alloca(buflen);
    buf[1] = STPP_SET_VOID;
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_DIR:
    buf = alloca(buflen);
    buf[1] = STPP_SET_DIR;
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_SET_RSTRING:
    str = rstr_get(va_arg(ap, rstr_t *));
    if(0)
    case PROP_SET_CSTRING:
      str = va_arg(ap, const char *);
    len = strlen(str);
    buflen += len + 1;
    buf = alloca(buflen);
    buf[1] = STPP_SET_STRING;
    buf[6] = event == PROP_SET_RSTRING ? va_arg(ap, int) : 0;
    memcpy(buf + 7, str, len);
    ss_clear_props(ss, &ss->ss_dir_props);
    break;

  case PROP_ADD_CHILD:
    buflen += 4;
    buf = alloca(buflen);
    wr32_le(buf + 6,
            stpp_property_export_from_sub(ss, va_arg(ap, prop_t *),
                                          &ss->ss_dir_props)->sp_id);

    flags = va_arg(ap, int);
    if(flags & PROP_ADD_SELECTED)
      buf[1] = STPP_ADD_CHILD_SELECTED;
    else
      buf[1] = STPP_ADD_CHILDS;
    break;

  case PROP_ADD_CHILD_BEFORE:
    p = va_arg(ap, prop_t *);
    before = va_arg(ap, prop_t *);

    buflen += 8;
    buf = alloca(buflen);
    wr32_le(buf + 6,  sp_get(before, ss)->sp_id);
    wr32_le(buf + 10, stpp_property_export_from_sub(ss, p,
                                                    &ss->ss_dir_props)->sp_id);
    buf[1] = STPP_ADD_CHILDS_BEFORE;
    break;

  case PROP_ADD_CHILD_VECTOR:
    pv = va_arg(ap, const prop_vec_t *);
    buflen += prop_vec_len(pv) * 4;
    buf = alloca(buflen);
    for(int i = 0; i < prop_vec_len(pv); i++) {
      wr32_le(buf + 6 + i * 4,
              stpp_property_export_from_sub(ss, prop_vec_get(pv, i),
                                            &ss->ss_dir_props)->sp_id);
    }
    buf[1] = STPP_ADD_CHILDS;
    break;

  case PROP_ADD_CHILD_VECTOR_BEFORE:
    pv = va_arg(ap, const prop_vec_t *);
    before = va_arg(ap, prop_t *);

    buflen += prop_vec_len(pv) * 4 + 4;
    buf = alloca(buflen);
    wr32_le(buf + 6,  sp_get(before, ss)->sp_id);
    for(int i = 0; i < prop_vec_len(pv); i++) {
      wr32_le(buf + 10 + i * 4,
              stpp_property_export_from_sub(ss, prop_vec_get(pv, i),
                                            &ss->ss_dir_props)->sp_id);
    }
    buf[1] = STPP_ADD_CHILDS_BEFORE;
    break;

  case PROP_DEL_CHILD:
    sp = prop_tag_clear(va_arg(ap, prop_t *), ss);
    buflen += 4;
    buf = alloca(buflen);
    wr32_le(buf + 6, sp->sp_id);
    buf[1] = STPP_DEL_CHILD;
    stpp_property_unexport_from_sub(ss, sp);
    break;

  case PROP_MOVE_CHILD:
    p = va_arg(ap, prop_t *);
    before = va_arg(ap, prop_t *);

    buflen += before ? 8 : 4;
    buf = alloca(buflen);

    wr32_le(buf + 6, sp_get(p, ss)->sp_id);
    if(before)
      wr32_le(buf + 10, sp_get(before, ss)->sp_id);
    buf[1] = STPP_MOVE_CHILD;
    break;

  case PROP_SELECT_CHILD:
    buflen += 4;
    buf = alloca(buflen);
    wr32_le(buf + 6, sp_get(va_arg(ap, prop_t *), ss)->sp_id);
    buf[1] = STPP_SELECT_CHILD;
    break;

  case PROP_VALUE_PROP:
    p = va_arg(ap, prop_t *);

    if(LIST_FIRST(&ss->ss_value_props) != NULL &&
       LIST_FIRST(&ss->ss_value_props)->sp_prop == p) {
      return;
    }

    ss_clear_props(ss, &ss->ss_value_props);

    buflen += 4;
    buf = alloca(buflen);

    wr32_le(buf + 6,
            stpp_property_export_from_sub(ss, p,
                                          &ss->ss_value_props)->sp_id);
    buf[1] = STPP_VALUE_PROP;
    break;

  case PROP_WANT_MORE_CHILDS:
    return;

  case PROP_HAVE_MORE_CHILDS_YES:
    buf = alloca(buflen);
    buf[1] = STPP_HAVE_MORE_CHILDS_YES;
    break;

  case PROP_HAVE_MORE_CHILDS_NO:
    buf = alloca(buflen);
    buf[1] = STPP_HAVE_MORE_CHILDS_NO;
    break;

  default:
    printf("STPP SUB BINARY cant handle event %d\n", event);
    return;
  }
  buf[0] = STPP_CMD_NOTIFY;
  wr32_le(buf + 2, ss->ss_id);
  websocket_send(hc, 2, buf, buflen);
}

/**
 *
 */
static void
stpp_cmd_sub(stpp_t *stpp, unsigned int id, int propref, const char *path,
             uint16_t flags,
             char **namevec,
             void (*notify)(void *opaque, prop_event_t event, ...))
{
  prop_t *p = resolve_propref(stpp, propref);
  stpp_subscription_t *ss = calloc(1, sizeof(stpp_subscription_t));

  ss->ss_id = id;
  if(RB_INSERT_SORTED(&stpp->stpp_subscriptions, ss, ss_link, ss_cmp)) {
    // ID Collision
    TRACE(TRACE_ERROR, "STPP", "Subscription ID %d already exist", id);
    free(ss);
    return;
  }

  ss->ss_stpp = stpp;
  ss->ss_sub = prop_subscribe(PROP_SUB_ALT_PATH | flags,
			      PROP_TAG_COURIER, asyncio_courier,
			      PROP_TAG_NAMESTR, path,
			      PROP_TAG_NAME_VECTOR, namevec,
			      PROP_TAG_CALLBACK, notify, ss,
			      PROP_TAG_ROOT, p,
			      NULL);
}


/**
 *
 */
static void
ss_destroy(stpp_t *stpp, stpp_subscription_t *ss)
{
  ss_clear_props(ss, &ss->ss_dir_props);
  ss_clear_props(ss, &ss->ss_value_props);
  prop_unsubscribe(ss->ss_sub);
  RB_REMOVE(&stpp->stpp_subscriptions, ss, ss_link);
  free(ss);
}


/**
 *
 */
static void
stpp_cmd_unsub(stpp_t *stpp, unsigned int id)
{
  stpp_subscription_t s, *ss;
  s.ss_id = id;
  
  if((ss = RB_FIND(&stpp->stpp_subscriptions, &s, ss_link, ss_cmp)) == NULL)
    return;
  ss_destroy(stpp, ss);
}


/**
 *
 */
static void
stpp_cmd_want_more_childs(stpp_t *stpp, unsigned int id)
{
  stpp_subscription_t s, *ss;
  s.ss_id = id;

  if((ss = RB_FIND(&stpp->stpp_subscriptions, &s, ss_link, ss_cmp)) == NULL)
    return;
  prop_want_more_childs(ss->ss_sub);
}


/**
 *
 */
static void
stpp_cmd_set(stpp_t *stpp, int propref, const char *path, htsmsg_field_t *v)
{
  if(path == NULL || v == NULL)
    return;

  prop_t *p = resolve_propref(stpp, propref);
  switch(v->hmf_type) {
  case HMF_S64:
    prop_setdn(NULL, p, path, PROP_SET_INT, v->hmf_s64);
    break;
  case HMF_STR:
    prop_setdn(NULL, p, path, PROP_SET_STRING, v->hmf_str);
    break;
  case HMF_DBL:
    prop_setdn(NULL, p, path, PROP_SET_FLOAT, v->hmf_dbl);
    break;
  }
}


/**
 *
 */
static void
stpp_json(stpp_t *stpp, htsmsg_t *m)
{
  int cmd = htsmsg_get_u32_or_default(m, HTSMSG_INDEX(0), 0);
  
  switch(cmd) {
  case STPP_CMD_SUBSCRIBE:
    stpp_cmd_sub(stpp,
		 htsmsg_get_u32_or_default(m, HTSMSG_INDEX(1), 0),
		 htsmsg_get_u32_or_default(m, HTSMSG_INDEX(2), 0),
		 htsmsg_get_str(m,            HTSMSG_INDEX(3)), 0, NULL,
                 stpp_sub_json);
    break;

  case STPP_CMD_UNSUBSCRIBE:
    stpp_cmd_unsub(stpp,
		   htsmsg_get_u32_or_default(m, HTSMSG_INDEX(1), 0));
    break;

  case STPP_CMD_SET:
    stpp_cmd_set(stpp,
		 htsmsg_get_u32_or_default(m, HTSMSG_INDEX(1), 0),
		 htsmsg_get_str(m, HTSMSG_INDEX(2)),
		 htsmsg_field_find(m, HTSMSG_INDEX(3)));
    break;
  }
}



/**
 *
 */
static int
decode_string_vector(char ***vp, const uint8_t *src, int remain)
{
  const uint8_t *s = src;
  int origlen = remain;
  *vp = NULL;
  int comp = 0;
  while(remain > 0) {
    remain -= *src + 1;
    if(*src == 0)
      break;
    src += *src + 1;
    comp++;
  }
  assert(remain >= 0);
  for(int i = 0; i < comp; i++) {
    int l = *s++;
    strvec_addpn(vp, (const char *)s, l);
    s += l;
  }
  return origlen - remain;
}


/**
 *
 */
static prop_t *
decode_propref(stpp_t *stpp, const uint8_t **datap, int *lenp)
{
  const uint8_t *data = *datap;
  int len = *lenp;
  if(len < 5)
    return NULL;

  prop_t *p = resolve_propref(stpp, rd32_le(data));
  if(p == NULL)
    return NULL;
  char **strvec = NULL;
  int x = decode_string_vector(&strvec, data + 4 , len - 4);
  *datap = data + 4 + x;
  *lenp  = len  - 4 - x;

  if(strvec == NULL)
    return prop_ref_inc(p);

  prop_t *p2 = prop_findv(p, strvec);
  strvec_free(strvec);
  return p2;
}


/**
 *
 */
static char *
decode_string(const uint8_t **datap, int *lenp)
{
  const uint8_t *data = *datap;
  int len = *lenp;

  if(len < 1)
    return NULL;
  int slen = data[0];
  int x;
  if(slen == 0xff) {
    if(len < 5)
      return NULL;
    slen = rd32_le(data + 1);
    x = 5;
  } else {
    x = 1;
  }
  data += x;
  len -= x;

  if(slen > len)
    return NULL;

  *datap = data + slen;
  *lenp  = len - slen;

  char *s = malloc(slen + 1);
  s[slen] = 0;
  memcpy(s, data, slen);
  return s;
}


/**
 *
 */
static void
stpp_binary_event(prop_t *p, event_type_t event,
                  const uint8_t *data, int len, stpp_t *stpp)
{
  event_t *e = NULL;
  switch(event) {
  case EVENT_ACTION_VECTOR:
    {
      char **strvec = NULL;
      int x = 0;

      decode_string_vector(&strvec, data, len);
      while(strvec[x] != NULL)
        x++;

      action_type_t *atv = alloca(x * sizeof(action_type_t));
      for(int i = 0; i < x; i++)
        atv[i] = action_str2code(strvec[i]);

      strvec_free(strvec);

      e = event_create_action_multi(atv, x);
    }
    break;
  case EVENT_OPENURL:
    {
      if(len < 1)
        break;
      uint8_t flags = data[0];
      data++;
      len--;

      event_openurl_args_t args = {0};

      if(flags & 0x01 && (args.url = decode_string(&data, &len)) == NULL)
        flags = 0;

      if(flags & 0x02 && (args.view = decode_string(&data, &len)) == NULL)
        flags = 0;

      if(flags & 0x04 &&
         (args.item_model = decode_propref(stpp, &data, &len)) == NULL)
        flags = 0;

      if(flags & 0x08 &&
         (args.parent_model = decode_propref(stpp, &data, &len)) == NULL)
        flags = 0;

      if(flags & 0x10 && (args.how = decode_string(&data, &len)) == NULL)
        flags = 0;

      if(flags & 0x20 && (args.parent_url = decode_string(&data, &len)) == NULL)
        flags = 0;

      if(flags)
        e = event_create_openurl_args(&args);


      free((void *)args.url);
      free((void *)args.view);
      free((void *)args.how);
      free((void *)args.parent_url);
      prop_ref_dec(args.item_model);
      prop_ref_dec(args.parent_model);
    }
    break;

  case EVENT_PLAYTRACK:
    {
      if(len < 1)
        break;
      uint8_t flags = data[0];
      data++;
      len--;

      prop_t *track = decode_propref(stpp, &data, &len);
      prop_t *model = NULL;
      if(flags & 0x01)
        model = decode_propref(stpp, &data, &len);

      int mode = len > 0 ? data[0] : 0;

      e = event_create_playtrack(track, model, mode);
      prop_ref_dec(track);
      prop_ref_dec(model);

    }
    break;

  case EVENT_DYNAMIC_ACTION:
    e = event_create_action_str((const char *)data);
    break;

  case EVENT_SELECT_AUDIO_TRACK:
  case EVENT_SELECT_SUBTITLE_TRACK:
    {
      if(len < 1)
        break;
      uint8_t flags = data[0];
      data++;
      len--;
      char *id = decode_string(&data, &len);
      if(id != NULL)
        e = event_create_select_track(id, event, flags & 1);
      free(id);
    }
    break;
  default:
    TRACE(TRACE_ERROR, "STPP", "Can't handle event type %d", event);
    return;
  }

  if(e == NULL)
    return;
  prop_send_ext_event(p, e);
  event_release(e);
}

static void
stpp_send_hello(stpp_t *stpp)
{
  int buflen = 1 + 1 + 16 + 1;
  uint8_t *buf = alloca(buflen);
  buf[0] = STPP_CMD_HELLO;
  buf[1] = STPP_VERSION;
  memcpy(buf + 2, gconf.running_instance, 16);
  buf[18] = 0x0; // Flags
  websocket_send(stpp->stpp_hc, 2, buf, buflen);
}

typedef struct stpp_imagereq {
  rstr_t *sir_url;
  uint32_t sir_id;
  uint32_t sir_req_width;
  uint32_t sir_req_height;
  uint32_t sir_flags;

  image_t *sir_image;

  LIST_ENTRY(stpp_imagereq) sir_link;
  stpp_t *sir_stpp;

  cancellable_t *sir_cancellable;

  char sir_errstr[256];

} stpp_imagereq_t;


static void
stpp_imagereq_send(void *aux)
{
  stpp_imagereq_t *sir = aux;
  stpp_t *stpp = sir->sir_stpp;
  if(stpp != NULL) {

    if(!cancellable_is_cancelled(sir->sir_cancellable)) {

      htsbuf_queue_t hq;
      htsbuf_queue_init(&hq, 0);
      image_t *im = sir->sir_image;
      if(im != NULL) {
        assert(im->im_num_components == 1);
        assert(im->im_components[0].type == IMAGE_CODED);
        buf_t *b = im->im_components[0].coded.icc_buf;

        uint8_t header[1 + 4 + 2 + 2 + 2 + 1 + 1 + 1];
        header[0] = STPP_CMD_IMAGE_REPLY;
        wr32_le(header + 1, sir->sir_id);
        wr16_le(header + 5, im->im_width);
        wr16_le(header + 7, im->im_height);
        wr16_le(header + 9, im->im_flags);
        header[11] = im->im_color_planes;
        header[12] = im->im_components[0].coded.icc_type;
        header[13] = im->im_orientation;

        htsbuf_append(&hq, header, sizeof(header));
        htsbuf_append_buf(&hq, b);

      } else {
        uint8_t header[5];

        header[0] = STPP_CMD_IMAGE_FAIL;
        wr32_le(header + 1, sir->sir_id);
        htsbuf_append(&hq, header, 5);
        htsbuf_append(&hq, sir->sir_errstr, strlen(sir->sir_errstr));
      }

      websocket_sendq(stpp->stpp_hc, 2, &hq);
    }
    LIST_REMOVE(sir, sir_link);
  }
  image_release(sir->sir_image);
  rstr_release(sir->sir_url);
  cancellable_release(sir->sir_cancellable);
  free(sir);
}


static void
stpp_imagereq_do(void *aux)
{
  stpp_imagereq_t *sir = aux;
  struct image_meta im = {

    .im_req_width = sir->sir_req_width,
    .im_req_height = sir->sir_req_height,
    .im_want_thumb = sir->sir_flags & 1,
    .im_no_decoding = 1,
  };

  sir->sir_image = backend_imageloader(sir->sir_url, &im,
                                       sir->sir_errstr, sizeof(sir->sir_errstr),
                                       NULL, sir->sir_cancellable, NULL);
  asyncio_run_task(stpp_imagereq_send, sir);
}




/**
 * Websocket server always pad frame with a zero byte at the end
 * (Not included in len). So it's safe to just use strings at end
 * of messages
 *
 * Function returns -1 on malformed messages
 */
static int
stpp_binary(stpp_t *stpp, const uint8_t *data, int len)
{
  union {
    float f;
    int i;
  } u;

  if(len < 1)
    return -1;
  const uint8_t cmd = data[0];
  data++;
  len--;

  if(cmd == STPP_CMD_HELLO) {
    if(len < 2)
      return -1;
#if 0
    uint8_t version = data[0];
    uint8_t flags = data[1];
    char *id = NULL;
    char *version = NULL;
#endif
    stpp_send_hello(stpp);
    stpp->stpp_helloed_ok = 1;
    return 0;
  }

  if(!stpp->stpp_helloed_ok)
    return -1;

  switch(cmd) {
  case STPP_CMD_SUBSCRIBE:
    if(len < 10)
      return -1;

    char **name = NULL;
    decode_string_vector(&name, data + 10, len - 10);

    stpp_cmd_sub(stpp, rd32_le(data), rd32_le(data + 4), NULL,
                 rd16_le(data + 8), name, stpp_sub_binary);

    strvec_free(name);
    break;

  case STPP_CMD_UNSUBSCRIBE:
    if(len != 4)
      return -1;
    stpp_cmd_unsub(stpp, rd32_le(data));
    break;


  case STPP_CMD_EVENT:
    {
      prop_t *p = decode_propref(stpp, &data, &len);
      if(p == NULL)
        return -1;

      if(p != NULL) {
        int event = data[0];
        data++;
        len--;
        stpp_binary_event(p, event, data, len, stpp);
      }
      prop_ref_dec(p);
    }
    break;

  case STPP_CMD_SET:
    {
      prop_t *p = decode_propref(stpp, &data, &len);
      if(p == NULL)
        break;

      if(len > 0) {
        uint8_t cmd = data[0];
        data++;
        len--;
        switch(cmd) {
        case STPP_SET_STRING:
          if(len < 1)
            break;
          prop_set_string_ex(p, NULL, (const char *)data + 1, data[0]);
          break;
        case STPP_SET_INT:
          if(len == 4)
            prop_set_int(p, rd32_le(data));
          break;
        case STPP_TOGGLE_INT:
          prop_toggle_int(p);
          break;
        case STPP_SET_VOID:
          prop_set_void(p);
          break;
        case STPP_SET_FLOAT:
          if(len != 4)
            break;
          u.i = rd32_le(data);
          prop_set_float(p, u.f);
          break;
        }
      }
      prop_ref_dec(p);
    }
    break;
  case STPP_CMD_REQ_MOVE:
    {
      if(len < 4)
        return -1;
      prop_t *p = resolve_propref(stpp, rd32_le(data));
      prop_t *before = NULL;
      if(len == 8)
        before = resolve_propref(stpp, rd32_le(data + 4));
      prop_req_move(p, before);
    }
    break;

  case STPP_CMD_WANT_MORE_CHILDS:
    if(len != 4)
      return -1;
    stpp_cmd_want_more_childs(stpp, rd32_le(data));
    break;

  case STPP_CMD_SELECT:
    {
      prop_t *p = decode_propref(stpp, &data, &len);
      if(p == NULL)
        break;

      prop_select(p);
    }
    break;

  case STPP_CMD_IMAGE_LOAD:
    {
      if(len < 4 * sizeof(uint32_t))
        return -1;
      uint32_t id = rd32_le(data + 0);
      uint32_t flags = rd32_le(data + 4);
      uint32_t req_width = rd32_le(data + 8);
      uint32_t req_height = rd32_le(data + 12);
      data += 16;
      len -= 16;

      stpp_imagereq_t *sir = calloc(1, sizeof(stpp_imagereq_t));
      sir->sir_cancellable = cancellable_create();
      sir->sir_url = rstr_alloc((const char *)data);
      sir->sir_id = id;
      sir->sir_req_width = req_width;
      sir->sir_req_height = req_height;
      sir->sir_flags = flags;
      sir->sir_stpp = stpp;
      LIST_INSERT_HEAD(&stpp->stpp_imagereqs, sir, sir_link);
      task_run(stpp_imagereq_do, sir);
    }
    break;

  case STPP_CMD_IMAGE_CANCEL:
    {
      if(len < sizeof(uint32_t))
        return -1;
      uint32_t id = rd32_le(data + 0);
      stpp_imagereq_t *sir;
      LIST_FOREACH(sir, &stpp->stpp_imagereqs, sir_link) {
        if(sir->sir_id == id) {
          cancellable_cancel(sir->sir_cancellable);
        }
      }
    }
    break;

  default:
    TRACE(TRACE_ERROR, "STPP", "Received bad command 0x%x", cmd);
    return -1;
  }
  return 0;
}

/**
 *
 */
static int
stpp_input(http_connection_t *hc, int opcode, 
	   uint8_t *data, size_t len, void *opaque)
{
  stpp_t *stpp = opaque;
  htsmsg_t *m;
  switch(opcode) {
  case 1: // Text frame
    m = htsmsg_json_deserialize((const char *)data);
    if(m != NULL) {
      stpp_json(stpp, m);
      htsmsg_release(m);
    }
    break;
  case 2: // Binary frame
    stpp_binary(stpp, data, len);
    break;

  default:
    break;
  }
  return 0;
}


/**
 *
 */
static int
stpp_init(http_connection_t *hc)
{
  stpp_t *stpp = calloc(1, sizeof(stpp_t));
  stpp->stpp_hc = hc;
  http_set_opaque(hc, stpp);
  return 0;
}


/**
 *
 */
static void
stpp_fini(http_connection_t *hc, void *opaque)
{
  stpp_t *stpp = opaque;

  while(stpp->stpp_subscriptions.root != NULL)
    ss_destroy(stpp, stpp->stpp_subscriptions.root);

  assert(stpp->stpp_props.root == NULL);

  stpp_imagereq_t *sir;
  while((sir = LIST_FIRST(&stpp->stpp_imagereqs)) != NULL) {
    LIST_REMOVE(sir, sir_link);
    sir->sir_stpp = NULL;
  }

  free(stpp);
}


/**
 *
 */
static void
ws_init(void)
{
  http_add_websocket("/api/stpp", stpp_init, stpp_input, stpp_fini);
}


INITME(INIT_GROUP_API, ws_init, NULL, 0);


/**
 *
 */
static int
be_stpp_open(prop_t *page, const char *url, int sync)
{
  prop_t *model = prop_create_r(page, "model");
  prop_set(model, "type", PROP_SET_STRING, "stpp");
  prop_ref_dec(model);
  return 0;
}


/**
 *
 */
static int
be_stpp_canhandle(const char *url)
{
  return !strncmp(url, "stpp://", strlen("stpp://"));
}


/**
 *
 */
static backend_t be_stpp = {
  .be_canhandle  = be_stpp_canhandle,
  .be_open       = be_stpp_open,
};

BE_REGISTER(stpp);
