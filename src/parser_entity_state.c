#include "parser_entity_state.h"
#include "arena.h"
#include "hashtable.h"
#include "utils.h"
#include <string.h>

typedef struct {
  size_t max_props;
  prop_exclude_set excluded_props;
  hashtable dts_with_excludes;
  size_t dt_index;
  size_t serverclass_index;
  size_t flattenedprop_index;
  uint16_t baseclass_array[1024];
  size_t baseclass_count;
  size_t baseclass_index;
} propdata;

static void add_baseclass(propdata *data, size_t dt_index) {
  ++data->baseclass_count;

  if (data->baseclass_count > 1024) {
    abort();
  }

  memmove(data->baseclass_array + data->baseclass_index + 1,
          data->baseclass_array + data->baseclass_index,
          sizeof(size_t) * data->baseclass_count - data->baseclass_index);
  data->baseclass_array[data->baseclass_index] = dt_index;
}

static uint16_t get_baseclass_from_array(propdata *data, size_t index) {
  return data->baseclass_array[index];
}

// Returns true if either hashtable gets completely full
static bool add_exclude(propdata *data, demogobbler_sendprop *prop, arena *a) {
  demogobbler_pes_insert(&data->excluded_props, prop);
  hashtable_entry entry;
  entry.str = prop->exclude_name;
  entry.value = 0;
  demogobbler_hashtable_insert(&data->dts_with_excludes, entry);
  return data->excluded_props.item_count != data->excluded_props.max_items &&
         data->dts_with_excludes.item_count != data->dts_with_excludes.max_items;
}

static bool does_datatable_have_excludes(propdata *data, demogobbler_sendtable *table) {
  hashtable_entry entry = demogobbler_hashtable_get(&data->dts_with_excludes, table->name);
  return entry.str != NULL;
}

static bool is_prop_excluded(propdata *data, demogobbler_sendtable *table,
                             demogobbler_sendprop *prop) {
  return demogobbler_pes_has(&data->excluded_props, table, prop);
}

static void create_dt_hashtable(parser *thisptr) {
  estate *entstate_ptr = &thisptr->state.entity_state;
  demogobbler_sendtable *sendtables = entstate_ptr->sendtables;
  const size_t sendtable_count = entstate_ptr->sendtable_count;
  const float FILL_RATE = 0.9f;
  const size_t buckets = (size_t)(sendtable_count / FILL_RATE);
  entstate_ptr->dt_hashtable = demogobbler_hashtable_create(buckets);
  for (size_t i = 0; i < sendtable_count && !thisptr->error; ++i) {
    hashtable_entry entry;
    entry.str = sendtables[i].name;
    entry.value = i;

    if (!demogobbler_hashtable_insert(&entstate_ptr->dt_hashtable, entry)) {
      thisptr->error = true;
      thisptr->error_message = "Hashtable collision with datatable names or ran out of space";
    }
  }
}

static size_t get_baseclass(parser *thisptr, propdata *data, demogobbler_sendprop *prop) {
  estate *entstate_ptr = &thisptr->state.entity_state;
  demogobbler_sendtable *sendtables = entstate_ptr->sendtables;
  if (prop->baseclass == NULL) {
    hashtable_entry entry = demogobbler_hashtable_get(&entstate_ptr->dt_hashtable, prop->dtname);

    if (entry.str == NULL) {
      // No entry found
      thisptr->error = true;
      thisptr->error_message = "Was unable to find datatable pointed to by sendprop";
      return 0;
    }
    prop->baseclass = sendtables + entry.value;
  }
  return prop->baseclass - sendtables;
}

static void gather_excludes(parser *thisptr, propdata *data, size_t datatable_index) {
  estate *entstate_ptr = &thisptr->state.entity_state;
  demogobbler_sendtable *table = entstate_ptr->sendtables + datatable_index;

  for (size_t prop_index = 0; prop_index < table->prop_count; ++prop_index) {
    demogobbler_sendprop *prop = table->props + prop_index;

    if (prop->proptype == sendproptype_datatable) {
      size_t baseclass_index = get_baseclass(thisptr, data, prop);

      if (thisptr->error)
        return;

      gather_excludes(thisptr, data, baseclass_index);
    } else if (prop->flag_exclude) {
      if (!add_exclude(data, prop, &thisptr->temp_arena)) {
        thisptr->error = true;
        thisptr->error_message = "Was unable to add exclude";
      }
    }
  }
}

static void gather_propdata(parser *thisptr, propdata *data, size_t datatable_index) {
  estate *entstate_ptr = &thisptr->state.entity_state;
  demogobbler_sendtable *sendtables = entstate_ptr->sendtables;
  demogobbler_sendtable *table = sendtables + datatable_index;
  bool table_has_excludes = does_datatable_have_excludes(data, table);

  for (size_t prop_index = 0; prop_index < table->prop_count; ++prop_index) {
    demogobbler_sendprop *prop = table->props + prop_index;
    bool prop_excluded = table_has_excludes && is_prop_excluded(data, table, prop);
    if (prop_excluded)
      continue;

    if (prop->proptype == sendproptype_datatable) {
      size_t baseclass_index = get_baseclass(thisptr, data, prop);

      if (thisptr->error)
        return;

      if (!prop->flag_collapsible) {
        add_baseclass(data, baseclass_index);
        gather_propdata(thisptr, data, baseclass_index);
        ++data->baseclass_index;
      } else {
        gather_propdata(thisptr, data, baseclass_index);
      }
    } else if (!prop->flag_insidearray && !prop->flag_exclude) {
      ++data->max_props;
    }
  }
}

static uint8_t get_priority_protocol4(demogobbler_sendprop *prop) {
  if (prop->priority >= 64 && prop->flag_changesoften)
    return 64;
  else
    return prop->priority;
}

static void sort_props(parser *thisptr, serverclass_data *class_data) {
  if (thisptr->demo_version.demo_protocol >= 4 && thisptr->demo_version.game != l4d) {
    bool priorities[256];
    memset(priorities, 0, sizeof(priorities));

    for (int i = 0; i < class_data->prop_count; ++i) {
      size_t prio = get_priority_protocol4(class_data->props + i);
      priorities[prio] = true;
    }

    size_t start = 0;

    for (size_t current_prio = 0; current_prio < 256; ++current_prio) {
      if (priorities[current_prio] == false)
        continue;

      for (size_t i = start; i < class_data->prop_count; ++i) {
        demogobbler_sendprop *prop = class_data->props + i;
        uint8_t prio = get_priority_protocol4(prop);

        if (prio == current_prio) {
          demogobbler_sendprop *dest = class_data->props + start;
          demogobbler_sendprop temp = *dest;
          *dest = *prop;
          *prop = temp;
          ++start;
        }
      }
    }
  } else {
    size_t start = 0;

    for (size_t i = start; i < class_data->prop_count; ++i) {
      demogobbler_sendprop *prop = class_data->props + i;
      if (prop->flag_changesoften) {
        demogobbler_sendprop *dest = class_data->props + start;
        demogobbler_sendprop temp = *dest;
        *dest = *prop;
        *prop = temp;
        ++start;
      }
    }
  }
}

static void iterate_props(parser *thisptr, propdata *data, demogobbler_sendtable *table) {
  bool table_has_excludes = does_datatable_have_excludes(data, table);
  estate *entstate_ptr = &thisptr->state.entity_state;
  for (size_t prop_index = 0; prop_index < table->prop_count; ++prop_index) {
    demogobbler_sendprop *prop = table->props + prop_index;
    if (prop->proptype == sendproptype_datatable) {
      if (prop->flag_collapsible)
        iterate_props(thisptr, data, prop->baseclass);
    } else if (!prop->flag_exclude && !prop->flag_insidearray) {
      bool prop_excluded = table_has_excludes && is_prop_excluded(data, table, prop);
      if (!prop_excluded) {
        size_t flattenedprop_index = entstate_ptr->class_datas[data->serverclass_index].prop_count;
        demogobbler_sendprop *dest =
            entstate_ptr->class_datas[data->serverclass_index].props + flattenedprop_index;
        memcpy(dest, prop, sizeof(demogobbler_sendprop));
        ++entstate_ptr->class_datas[data->serverclass_index].prop_count;
      }
    }
  }
}

static void gather_props(parser *thisptr, propdata *data) {
  estate *entstate_ptr = &thisptr->state.entity_state;
  demogobbler_sendtable *sendtables = entstate_ptr->sendtables;
  for (size_t i = 0; i < data->baseclass_count; ++i) {
    uint16_t baseclass_index = get_baseclass_from_array(data, i);
    demogobbler_sendtable *table = sendtables + baseclass_index;
    iterate_props(thisptr, data, table);
  }

  iterate_props(thisptr, data, sendtables + data->dt_index);
}

#define CHECK_ERR()                                                                                \
  if (thisptr->error)                                                                              \
  goto end

static void parse_serverclass(parser *thisptr, size_t i) {
  estate *entstate_ptr = &thisptr->state.entity_state;
  // Reset iteration state
  propdata data;
  memset(&data, 0, sizeof(propdata));
  data.excluded_props = thisptr->state.entity_state.excluded_props;
  data.dts_with_excludes = thisptr->state.entity_state.dts_with_excludes;
  data.serverclass_index = i;

  demogobbler_serverclass *cls = entstate_ptr->serverclasses + i;
  hashtable_entry entry =
      demogobbler_hashtable_get(&entstate_ptr->dt_hashtable, cls->datatable_name);
  data.dt_index = entry.value;

  if (entry.str == NULL) {
    thisptr->error = true;
    thisptr->error_message = "No datatable found for serverclass";
    goto end;
  }

  data.dts_with_excludes.item_count = 0;
  demogobbler_pes_clear(&data.excluded_props);
  gather_excludes(thisptr, &data, data.dt_index);
  CHECK_ERR();
  gather_propdata(thisptr, &data, data.dt_index);
  CHECK_ERR();

  entstate_ptr->class_datas[i].props = demogobbler_arena_allocate(
      &thisptr->memory_arena, sizeof(demogobbler_sendprop) * data.max_props,
      alignof(demogobbler_sendprop));
  entstate_ptr->class_datas[i].prop_count = 0;
  entstate_ptr->class_datas[i].dt_name = (entstate_ptr->sendtables + data.dt_index)->name;

  gather_props(thisptr, &data);
  CHECK_ERR();
  sort_props(thisptr, entstate_ptr->class_datas + i);
  CHECK_ERR();
end:;
}

void demogobbler_parser_init_estate(parser *thisptr) {
  estate *entstate_ptr = &thisptr->state.entity_state;
  demogobbler_sendtable *sendtables = entstate_ptr->sendtables;
  entstate_ptr->edicts = demogobbler_arena_allocate(&thisptr->memory_arena,
                                                    sizeof(edict) * MAX_EDICTS, alignof(edict));
  memset(entstate_ptr->edicts, 0, sizeof(edict) * MAX_EDICTS);
  create_dt_hashtable(thisptr);

  CHECK_ERR();

  entstate_ptr->excluded_props = demogobbler_pes_create(256);
  entstate_ptr->dts_with_excludes = demogobbler_hashtable_create(256);
  size_t array_size = sizeof(serverclass_data) * entstate_ptr->serverclass_count;
  entstate_ptr->class_datas =
      demogobbler_arena_allocate(&thisptr->memory_arena, array_size, alignof(serverclass_data));
  memset(entstate_ptr->class_datas, 0, array_size);

  if (thisptr->m_settings.flattened_props_handler) {
    for (size_t i = 0; i < entstate_ptr->serverclass_count; ++i) {
      parse_serverclass(thisptr, i);
    }
    thisptr->m_settings.flattened_props_handler(&thisptr->state);
  }

end:;
}

serverclass_data* demogobbler_estate_serverclass_data(parser* thisptr, size_t index) {
  estate *entstate_ptr = &thisptr->state.entity_state;
  if(entstate_ptr->class_datas[index].dt_name == NULL) {
    parse_serverclass(thisptr, index);
  }
  
  return entstate_ptr->class_datas + index;
}
