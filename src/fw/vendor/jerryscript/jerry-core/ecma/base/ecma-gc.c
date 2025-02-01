/* Copyright 2014-2016 Samsung Electronics Co., Ltd.
 * Copyright 2016 University of Szeged.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Garbage collector implementation
 */

#include "ecma-alloc.h"
#include "ecma-globals.h"
#include "ecma-gc.h"
#include "ecma-helpers.h"
#include "ecma-lcache.h"
#include "ecma-property-hashmap.h"
#include "jcontext.h"
#include "jrt.h"
#include "jrt-libc-includes.h"
#include "jrt-bit-fields.h"
#include "re-compiler.h"
#include "vm-defines.h"
#include "vm-stack.h"

#define JERRY_INTERNAL
#include "jerry-internal.h"

/* TODO: Extract GC to a separate component */

/** \addtogroup ecma ECMA
 * @{
 *
 * \addtogroup ecmagc Garbage collector
 * @{
 */

/**
 * Current state of an object's visited flag that
 * indicates whether the object is in visited state:
 *
 *  visited_field | visited_flip_flag | real_value
 *         false  |            false  |     false
 *         false  |             true  |      true
 *          true  |            false  |      true
 *          true  |             true  |     false
 */

static void ecma_gc_mark (ecma_object_t *object_p);
static void ecma_gc_sweep (ecma_object_t *object_p);

/**
 * Get next object in list of objects with same generation.
 */
static inline ecma_object_t *
ecma_gc_get_object_next (ecma_object_t *object_p) /**< object */
{
  JERRY_ASSERT (object_p != NULL);

  return ECMA_GET_POINTER (ecma_object_t, object_p->gc_next_cp);
} /* ecma_gc_get_object_next */

/**
 * Set next object in list of objects with same generation.
 */
static inline void
ecma_gc_set_object_next (ecma_object_t *object_p, /**< object */
                         ecma_object_t *next_object_p) /**< next object */
{
  JERRY_ASSERT (object_p != NULL);

  ECMA_SET_POINTER (object_p->gc_next_cp, next_object_p);
} /* ecma_gc_set_object_next */

/**
 * Get visited flag of the object.
 */
static inline bool
ecma_gc_is_object_visited (ecma_object_t *object_p) /**< object */
{
  JERRY_ASSERT (object_p != NULL);

  bool flag_value = (object_p->type_flags_refs & ECMA_OBJECT_FLAG_GC_VISITED) != 0;

  return flag_value != JERRY_CONTEXT (ecma_gc_visited_flip_flag);
} /* ecma_gc_is_object_visited */

/**
 * Set visited flag of the object.
 */
static inline void
ecma_gc_set_object_visited (ecma_object_t *object_p, /**< object */
                            bool is_visited) /**< flag value */
{
  JERRY_ASSERT (object_p != NULL);

  if (is_visited != JERRY_CONTEXT (ecma_gc_visited_flip_flag))
  {
    object_p->type_flags_refs = (uint16_t) (object_p->type_flags_refs | ECMA_OBJECT_FLAG_GC_VISITED);
  }
  else
  {
    object_p->type_flags_refs = (uint16_t) (object_p->type_flags_refs & ~ECMA_OBJECT_FLAG_GC_VISITED);
  }
} /* ecma_gc_set_object_visited */

/**
 * Initialize GC information for the object
 */
inline void
ecma_init_gc_info (ecma_object_t *object_p) /**< object */
{
  JERRY_CONTEXT (ecma_gc_objects_number)++;
  JERRY_CONTEXT (ecma_gc_new_objects)++;

  JERRY_ASSERT (JERRY_CONTEXT (ecma_gc_new_objects) <= JERRY_CONTEXT (ecma_gc_objects_number));

  JERRY_ASSERT (object_p->type_flags_refs < ECMA_OBJECT_REF_ONE);
  object_p->type_flags_refs = (uint16_t) (object_p->type_flags_refs | ECMA_OBJECT_REF_ONE);

  ecma_gc_set_object_next (object_p, JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_WHITE_GRAY]);
  JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_WHITE_GRAY] = object_p;

  /* Should be set to false at the beginning of garbage collection */
  ecma_gc_set_object_visited (object_p, false);
} /* ecma_init_gc_info */

/**
 * Increase reference counter of an object
 */
void
ecma_ref_object (ecma_object_t *object_p) /**< object */
{
  if (likely (object_p->type_flags_refs < ECMA_OBJECT_MAX_REF))
  {
    object_p->type_flags_refs = (uint16_t) (object_p->type_flags_refs + ECMA_OBJECT_REF_ONE);
  }
  else
  {
    jerry_fatal (ERR_REF_COUNT_LIMIT);
  }
} /* ecma_ref_object */

/**
 * Decrease reference counter of an object
 */
void
ecma_deref_object (ecma_object_t *object_p) /**< object */
{
  JERRY_ASSERT (object_p->type_flags_refs >= ECMA_OBJECT_REF_ONE);
  object_p->type_flags_refs = (uint16_t) (object_p->type_flags_refs - ECMA_OBJECT_REF_ONE);
} /* ecma_deref_object */

/**
 * Mark referenced object from property
 */
static void
ecma_gc_mark_property (ecma_property_t *property_p) /**< property */
{
  switch (ECMA_PROPERTY_GET_TYPE (*property_p))
  {
    case ECMA_PROPERTY_TYPE_NAMEDDATA:
    {
      ecma_value_t value = ECMA_PROPERTY_VALUE_PTR (property_p)->value;

      if (ecma_is_value_object (value))
      {
        ecma_object_t *value_obj_p = ecma_get_object_from_value (value);

        ecma_gc_set_object_visited (value_obj_p, true);
      }
      break;
    }
    case ECMA_PROPERTY_TYPE_NAMEDACCESSOR:
    {
      ecma_property_value_t *prop_value_p = ECMA_PROPERTY_VALUE_PTR (property_p);
      ecma_object_t *getter_obj_p = ecma_get_named_accessor_property_getter (prop_value_p);
      ecma_object_t *setter_obj_p = ecma_get_named_accessor_property_setter (prop_value_p);

      if (getter_obj_p != NULL)
      {
        ecma_gc_set_object_visited (getter_obj_p, true);
      }

      if (setter_obj_p != NULL)
      {
        ecma_gc_set_object_visited (setter_obj_p, true);
      }
      break;
    }
    case ECMA_PROPERTY_TYPE_INTERNAL:
    {
      uint32_t property_value = ECMA_PROPERTY_VALUE_PTR (property_p)->value;

      switch (ECMA_PROPERTY_GET_INTERNAL_PROPERTY_TYPE (property_p))
      {
        case ECMA_INTERNAL_PROPERTY_ECMA_VALUE: /* an ecma_value_t except object */
        case ECMA_INTERNAL_PROPERTY_DATE_FLOAT: /* pointer to a ecma_number_t */
        case ECMA_INTERNAL_PROPERTY_CLASS: /* an enum */
        case ECMA_INTERNAL_PROPERTY_REGEXP_BYTECODE: /* pointer to a regexp bytecode array */
        case ECMA_INTERNAL_PROPERTY_NATIVE_HANDLE: /* an external pointer */
        case ECMA_INTERNAL_PROPERTY_FREE_CALLBACK: /* an object's native free callback */
        case ECMA_INTERNAL_PROPERTY_INSTANTIATED_MASK_32_63: /* an integer (bit-mask) */
        {
          break;
        }

        case ECMA_INTERNAL_PROPERTY_BOUND_FUNCTION_BOUND_THIS: /* an ecma value */
        {
          if (ecma_is_value_object (property_value))
          {
            ecma_object_t *obj_p = ecma_get_object_from_value (property_value);

            ecma_gc_set_object_visited (obj_p, true);
          }

          break;
        }

        case ECMA_INTERNAL_PROPERTY_BOUND_FUNCTION_BOUND_ARGS: /* a collection of ecma values */
        {
          ecma_collection_header_t *bound_arg_list_p = ECMA_GET_INTERNAL_VALUE_POINTER (ecma_collection_header_t,
                                                                                        property_value);

          ecma_collection_iterator_t bound_args_iterator;
          ecma_collection_iterator_init (&bound_args_iterator, bound_arg_list_p);

          for (ecma_length_t i = 0; i < bound_arg_list_p->unit_number; i++)
          {
            bool is_moved = ecma_collection_iterator_next (&bound_args_iterator);
            JERRY_ASSERT (is_moved);

            if (ecma_is_value_object (*bound_args_iterator.current_value_p))
            {
              ecma_object_t *obj_p = ecma_get_object_from_value (*bound_args_iterator.current_value_p);

              ecma_gc_set_object_visited (obj_p, true);
            }
          }

          break;
        }

        case ECMA_INTERNAL_PROPERTY_BOUND_FUNCTION_TARGET_FUNCTION: /* an object */
        case ECMA_INTERNAL_PROPERTY_SCOPE: /* a lexical environment */
        case ECMA_INTERNAL_PROPERTY_PARAMETERS_MAP: /* an object */
        {
          ecma_object_t *obj_p = ECMA_GET_INTERNAL_VALUE_POINTER (ecma_object_t, property_value);

          ecma_gc_set_object_visited (obj_p, true);

          break;
        }
        case ECMA_INTERNAL_PROPERTY__COUNT: /* not a real internal property type,
                                             * but number of the real internal property types */
        {
          JERRY_UNREACHABLE ();
          break;
        }
      }
      break;
    }
    default:
    {
      JERRY_UNREACHABLE ();
      break;
    }
  }
} /* ecma_gc_mark_property */

/**
 * Mark objects as visited starting from specified object as root
 */
void
ecma_gc_mark (ecma_object_t *object_p) /**< object to mark from */
{
  JERRY_ASSERT (object_p != NULL);
  JERRY_ASSERT (ecma_gc_is_object_visited (object_p));

  bool traverse_properties = true;

  if (ecma_is_lexical_environment (object_p))
  {
    ecma_object_t *lex_env_p = ecma_get_lex_env_outer_reference (object_p);
    if (lex_env_p != NULL)
    {
      ecma_gc_set_object_visited (lex_env_p, true);
    }

    if (ecma_get_lex_env_type (object_p) != ECMA_LEXICAL_ENVIRONMENT_DECLARATIVE)
    {
      ecma_object_t *binding_object_p = ecma_get_lex_env_binding_object (object_p);
      ecma_gc_set_object_visited (binding_object_p, true);

      traverse_properties = false;
    }
  }
  else
  {
    ecma_object_t *proto_p = ecma_get_object_prototype (object_p);
    if (proto_p != NULL)
    {
      ecma_gc_set_object_visited (proto_p, true);
    }

    if (!ecma_get_object_is_builtin (object_p)
        && ecma_get_object_type (object_p) == ECMA_OBJECT_TYPE_FUNCTION)
    {
      ecma_extended_object_t *ext_func_p = (ecma_extended_object_t *) object_p;

      ecma_object_t *scope_p = ECMA_GET_INTERNAL_VALUE_POINTER (ecma_object_t,
                                                                ext_func_p->u.function.scope_cp);

      ecma_gc_set_object_visited (scope_p, true);
    }
  }

  if (traverse_properties)
  {
    ecma_property_header_t *prop_iter_p = ecma_get_property_list (object_p);

    if (prop_iter_p != NULL
        && ECMA_PROPERTY_GET_TYPE (prop_iter_p->types[0]) == ECMA_PROPERTY_TYPE_HASHMAP)
    {
      prop_iter_p = ECMA_GET_POINTER (ecma_property_header_t,
                                      prop_iter_p->next_property_cp);
    }

    while (prop_iter_p != NULL)
    {
      JERRY_ASSERT (ECMA_PROPERTY_IS_PROPERTY_PAIR (prop_iter_p));

      if (prop_iter_p->types[0] != ECMA_PROPERTY_TYPE_DELETED)
      {
        ecma_gc_mark_property (prop_iter_p->types + 0);
      }

      if (prop_iter_p->types[1] != ECMA_PROPERTY_TYPE_DELETED)
      {
        ecma_gc_mark_property (prop_iter_p->types + 1);
      }

      prop_iter_p = ECMA_GET_POINTER (ecma_property_header_t,
                                      prop_iter_p->next_property_cp);
    }
  }
} /* ecma_gc_mark */

/**
 * Free specified object
 */
void
ecma_gc_sweep (ecma_object_t *object_p) /**< object to free */
{
  JERRY_ASSERT (object_p != NULL
                && !ecma_gc_is_object_visited (object_p)
                && object_p->type_flags_refs < ECMA_OBJECT_REF_ONE);

  if (!ecma_is_lexical_environment (object_p))
  {
    /* if the object provides free callback, invoke it with handle stored in the object */

    ecma_external_pointer_t freecb_p;
    ecma_external_pointer_t native_p;

    bool is_retrieved = ecma_get_external_pointer_value (object_p,
                                                         ECMA_INTERNAL_PROPERTY_FREE_CALLBACK,
                                                         &freecb_p);

    if (is_retrieved && ((jerry_object_free_callback_t) freecb_p) != NULL)
    {
      is_retrieved = ecma_get_external_pointer_value (object_p,
                                                      ECMA_INTERNAL_PROPERTY_NATIVE_HANDLE,
                                                      &native_p);
      JERRY_ASSERT (is_retrieved);

      jerry_dispatch_object_free_callback (freecb_p, native_p);
    }
  }

  if (!ecma_is_lexical_environment (object_p)
      || ecma_get_lex_env_type (object_p) == ECMA_LEXICAL_ENVIRONMENT_DECLARATIVE)
  {
    ecma_property_header_t *prop_iter_p = ecma_get_property_list (object_p);

    if (prop_iter_p != NULL
        && ECMA_PROPERTY_GET_TYPE (prop_iter_p->types[0]) == ECMA_PROPERTY_TYPE_HASHMAP)
    {
      ecma_property_hashmap_free (object_p);
      prop_iter_p = ecma_get_property_list (object_p);
    }

    while (prop_iter_p != NULL)
    {
      JERRY_ASSERT (ECMA_PROPERTY_IS_PROPERTY_PAIR (prop_iter_p));

      /* Both cannot be deleted. */
      JERRY_ASSERT (prop_iter_p->types[0] != ECMA_PROPERTY_TYPE_DELETED
                    || prop_iter_p->types[1] != ECMA_PROPERTY_TYPE_DELETED);

      ecma_property_pair_t *prop_pair_p = (ecma_property_pair_t *) prop_iter_p;

      for (int i = 0; i < ECMA_PROPERTY_PAIR_ITEM_COUNT; i++)
      {
        if (prop_iter_p->types[i] != ECMA_PROPERTY_TYPE_DELETED)
        {
          ecma_string_t *name_p = ECMA_GET_POINTER (ecma_string_t, prop_pair_p->names_cp[i]);

          ecma_free_property (object_p, name_p, prop_iter_p->types + i);

          if (name_p != NULL)
          {
            ecma_deref_ecma_string (name_p);
          }
        }
      }

      /* Both must be deleted. */
      JERRY_ASSERT (prop_iter_p->types[0] == ECMA_PROPERTY_TYPE_DELETED
                    && prop_iter_p->types[1] == ECMA_PROPERTY_TYPE_DELETED);

      prop_iter_p = ECMA_GET_POINTER (ecma_property_header_t,
                                      prop_iter_p->next_property_cp);

      ecma_dealloc_property_pair (prop_pair_p);
    }
  }

  JERRY_ASSERT (JERRY_CONTEXT (ecma_gc_objects_number) > 0);
  JERRY_CONTEXT (ecma_gc_objects_number)--;

  if (!ecma_is_lexical_environment (object_p))
  {
    if (ecma_get_object_is_builtin (object_p)
        || ecma_get_object_type (object_p) == ECMA_OBJECT_TYPE_EXTERNAL_FUNCTION)
    {
      ecma_dealloc_extended_object ((ecma_extended_object_t *) object_p);
      return;
    }

    if (ecma_get_object_type (object_p) == ECMA_OBJECT_TYPE_FUNCTION)
    {
      /* Function with byte-code (not a built-in function). */
      ecma_extended_object_t *ext_func_p = (ecma_extended_object_t *) object_p;

      ecma_bytecode_deref (ECMA_GET_INTERNAL_VALUE_POINTER (ecma_compiled_code_t,
                                                            ext_func_p->u.function.bytecode_cp));

      ecma_dealloc_extended_object (ext_func_p);
      return;
    }
  }

  ecma_dealloc_object (object_p);
} /* ecma_gc_sweep */

/**
 * Run garbage collection
 */
void
ecma_gc_run (jmem_free_unused_memory_severity_t severity) /**< gc severity */
{
  JERRY_CONTEXT (ecma_gc_new_objects) = 0;

  JERRY_ASSERT (JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_BLACK] == NULL);

  /* if some object is referenced from stack or globals (i.e. it is root), mark it */
  for (ecma_object_t *obj_iter_p = JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_WHITE_GRAY];
       obj_iter_p != NULL;
       obj_iter_p = ecma_gc_get_object_next (obj_iter_p))
  {
    JERRY_ASSERT (!ecma_gc_is_object_visited (obj_iter_p));

    if (obj_iter_p->type_flags_refs >= ECMA_OBJECT_REF_ONE)
    {
      ecma_gc_set_object_visited (obj_iter_p, true);
    }
  }

  bool marked_anything_during_current_iteration = false;

  do
  {
    marked_anything_during_current_iteration = false;

    ecma_object_t *obj_prev_p = NULL;
    ecma_object_t *obj_iter_p = JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_WHITE_GRAY];

    while (obj_iter_p != NULL)
    {
      ecma_object_t *obj_next_p = ecma_gc_get_object_next (obj_iter_p);

      if (ecma_gc_is_object_visited (obj_iter_p))
      {
        /* Moving the object to list of marked objects */
        ecma_gc_set_object_next (obj_iter_p, JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_BLACK]);
        JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_BLACK] = obj_iter_p;

        if (likely (obj_prev_p != NULL))
        {
          JERRY_ASSERT (ecma_gc_get_object_next (obj_prev_p) == obj_iter_p);

          ecma_gc_set_object_next (obj_prev_p, obj_next_p);
        }
        else
        {
          JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_WHITE_GRAY] = obj_next_p;
        }

        ecma_gc_mark (obj_iter_p);
        marked_anything_during_current_iteration = true;
      }
      else
      {
        obj_prev_p = obj_iter_p;
      }

      obj_iter_p = obj_next_p;
    }
  }
  while (marked_anything_during_current_iteration);

  /* Sweeping objects that are currently unmarked */
  ecma_object_t *obj_iter_p = JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_WHITE_GRAY];

  while (obj_iter_p != NULL)
  {
    ecma_object_t *obj_next_p = ecma_gc_get_object_next (obj_iter_p);

    JERRY_ASSERT (!ecma_gc_is_object_visited (obj_iter_p));

    ecma_gc_sweep (obj_iter_p);
    obj_iter_p = obj_next_p;
  }

  if (severity == JMEM_FREE_UNUSED_MEMORY_SEVERITY_HIGH)
  {
    /* Remove the property hashmap of BLACK objects */
    obj_iter_p = JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_BLACK];

    while (obj_iter_p != NULL)
    {
      JERRY_ASSERT (ecma_gc_is_object_visited (obj_iter_p));

      if (!ecma_is_lexical_environment (obj_iter_p)
          || ecma_get_lex_env_type (obj_iter_p) == ECMA_LEXICAL_ENVIRONMENT_DECLARATIVE)
      {
        ecma_property_header_t *prop_iter_p = ecma_get_property_list (obj_iter_p);
        if (prop_iter_p != NULL
            && ECMA_PROPERTY_GET_TYPE (prop_iter_p->types[0]) == ECMA_PROPERTY_TYPE_HASHMAP)
        {
          ecma_property_hashmap_free (obj_iter_p);
        }
      }

      obj_iter_p = ecma_gc_get_object_next (obj_iter_p);
    }
  }

  /* Unmarking all objects */
  ecma_object_t *black_objects = JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_BLACK];
  JERRY_CONTEXT (ecma_gc_objects_lists)[ECMA_GC_COLOR_WHITE_GRAY] = black_objects;
  JERRY_CONTEXT (ecma_gc_objects_lists) [ECMA_GC_COLOR_BLACK] = NULL;

  JERRY_CONTEXT (ecma_gc_visited_flip_flag) = !JERRY_CONTEXT (ecma_gc_visited_flip_flag);

#ifndef CONFIG_DISABLE_REGEXP_BUILTIN
  /* Free RegExp bytecodes stored in cache */
  re_cache_gc_run ();
#endif /* !CONFIG_DISABLE_REGEXP_BUILTIN */
} /* ecma_gc_run */

/**
 * Try to free some memory (depending on severity).
 */
void
ecma_free_unused_memory (jmem_free_unused_memory_severity_t severity, /**< severity of the request */
                         size_t requested_size_bytes, /**< number of bytes to be allocated */
                         bool fatal_if_not_freed) /**< run-time will terminate if not enough memory is freed */
{
  if (severity == JMEM_FREE_UNUSED_MEMORY_SEVERITY_LOW)
  {
    /*
     * If there is enough newly allocated objects since last GC, probably it is worthwhile to start GC now.
     * Otherwise, probability to free sufficient space is considered to be low.
     */
    size_t new_objects_share = CONFIG_ECMA_GC_NEW_OBJECTS_SHARE_TO_START_GC;

    if (JERRY_CONTEXT (ecma_gc_new_objects) * new_objects_share > JERRY_CONTEXT (ecma_gc_objects_number))
    {
      ecma_gc_run (severity);
    }
  }
  else
  {
    JERRY_ASSERT (severity == JMEM_FREE_UNUSED_MEMORY_SEVERITY_HIGH);

    /* Freeing as much memory as we currently can */
    ecma_gc_run (severity);
  }
} /* ecma_free_unused_memory */

/**
 * @}
 * @}
 */
