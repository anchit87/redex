/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApiLevelsUtils.h"

#include <boost/algorithm/string.hpp>
#include <fstream>

#include "DexClass.h"
#include "TypeReference.h"
#include "TypeSystem.h"

namespace api {

/**
 * File format:
 *  <framework_cls> <num_methods> <num_fields>
 *      M <method0>
 *      M <method1>
 *      ...
 *      F <field0>
 *      F <field1>
 *      ...
 */
std::unordered_map<DexType*, FrameworkAPI>
ApiLevelsUtils::get_framework_classes() {
  std::ifstream infile(m_framework_api_info_filename.c_str());
  assert_log(infile, "Failed to open framework api file: %s\n",
             m_framework_api_info_filename.c_str());

  FrameworkAPI framework_api;
  std::string framework_cls_str;
  std::string class_name;
  uint32_t num_methods;
  uint32_t num_fields;

  std::unordered_map<DexType*, FrameworkAPI> framework_cls_to_api;

  while (infile >> framework_cls_str >> num_methods >> num_fields) {
    framework_api.cls = DexType::make_type(framework_cls_str.c_str());
    always_assert_log(framework_cls_to_api.count(framework_api.cls) == 0,
                      "Duplicated class name!");

    while (num_methods-- > 0) {
      std::string method_str;
      std::string tag;
      infile >> tag >> method_str;

      always_assert(tag == "M");
      DexMethodRef* mref = DexMethod::make_method(method_str);
      framework_api.mrefs.emplace(mref);
    }

    while (num_fields-- > 0) {
      std::string field_str;
      std::string tag;
      infile >> tag >> field_str;

      always_assert(tag == "F");
      DexFieldRef* fref = DexField::make_field(field_str);
      framework_api.frefs.emplace(fref);
    }

    framework_cls_to_api[framework_api.cls] = std::move(framework_api);
  }

  return framework_cls_to_api;
}

namespace {

/**
 * Lcom/facebook/something/ClassName$Foo; -> ClassName$Foo
 *
 * TODO(emmasevastian): Move it to utils.
 */
std::string get_simple_deobfuscated_name(DexType* type) {
  auto* cls = type_class(type);
  std::string full_name = "";
  if (cls) {
    full_name = cls->get_deobfuscated_name();
  }
  if (full_name.empty()) {
    full_name = type->str();
  }

  size_t simple_name_pos = full_name.rfind("/");
  always_assert(simple_name_pos != std::string::npos);
  return full_name.substr(simple_name_pos + 1,
                          full_name.size() - simple_name_pos - 2);
}

/**
 * This utils handles both:
 * - filtering of types with the same simple name
 * - creation of mapping from simple_name to type
 */
std::unordered_map<std::string, DexType*> get_simple_cls_name_to_accepted_types(
    const std::unordered_map<DexType*, FrameworkAPI>& framework_cls_to_api) {

  std::vector<std::string> filter;
  std::unordered_map<std::string, DexType*> simple_cls_name_to_type;
  for (const auto& pair : framework_cls_to_api) {
    auto simple_name = get_simple_deobfuscated_name(pair.first);

    // For now, excluding types that have the same simple name.
    // TODO(emmasevastian): Hacky! Do this better!
    const auto& inserted =
        simple_cls_name_to_type.emplace(simple_name, pair.first);
    bool insertion_happened = inserted.second;
    if (!insertion_happened) {
      filter.emplace_back(simple_name);
    }
  }

  for (const std::string& str : filter) {
    simple_cls_name_to_type.erase(str);
  }

  return simple_cls_name_to_type;
}

} // namespace

namespace {

bool find_method(DexString* meth_name,
                 DexProto* meth_proto,
                 const std::unordered_set<DexMethodRef*>& mrefs) {
  for (DexMethodRef* mref : mrefs) {
    if (mref->get_name() == meth_name && mref->get_proto() == meth_proto) {
      return true;
    }
  }

  return false;
}

/**
 * When checking if a method of a release class exists in the framework
 * equivalent, checking directly the replaced version (as in replacing all
 * arguments / return value that will be replaced in the end).
 */
bool check_methods(
    const std::vector<DexMethod*>& methods,
    const api::FrameworkAPI& framework_api,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework) {
  if (methods.size() == 0) {
    return true;
  }

  DexType* current_type = methods.at(0)->get_class();
  for (DexMethod* meth : methods) {
    if (!is_public(meth)) {
      // TODO(emmasevastian): When should we check non-public methods?
      continue;
    }

    auto* new_proto =
        type_reference::get_new_proto(meth->get_proto(), release_to_framework);
    // NOTE: For now, this assumes no obfuscation happened. We need to update
    //       it, if it runs later.
    if (!find_method(meth->get_name(), new_proto, framework_api.mrefs)) {
      return false;
    }
  }

  return true;
}

bool find_field(DexString* field_name,
                DexType* field_type,
                const std::unordered_set<DexFieldRef*>& frefs) {
  for (auto* fref : frefs) {
    if (fref->get_name() == field_name && fref->get_type() == field_type) {
      return true;
    }
  }

  return false;
}

bool check_fields(
    const std::vector<DexField*>& fields,
    const api::FrameworkAPI& framework_api,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework) {
  if (fields.size() == 0) {
    return true;
  }

  DexType* current_type = fields.at(0)->get_class();
  for (DexField* field : fields) {
    if (!is_public(field)) {
      // TODO(emmasevastian): When should we check non-public fields?
      continue;
    }

    auto* field_type = field->get_type();
    auto it = release_to_framework.find(field_type);

    auto* new_field_type = field_type;
    if (it != release_to_framework.end()) {
      new_field_type = it->second;
    }

    if (!find_field(field->get_name(), new_field_type, framework_api.frefs)) {
      return false;
    }
  }

  return true;
}

/**
 * Checks that all public members (for now) of release class, exist in
 * compatibility class.
 */
bool check_members(
    DexClass* cls,
    const api::FrameworkAPI& framework_api,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework) {
  if (!check_methods(cls->get_dmethods(), framework_api,
                     release_to_framework)) {
    return false;
  }
  if (!check_methods(cls->get_vmethods(), framework_api,
                     release_to_framework)) {
    return false;
  }

  if (!check_fields(cls->get_sfields(), framework_api, release_to_framework)) {
    return false;
  }
  if (!check_fields(cls->get_ifields(), framework_api, release_to_framework)) {
    return false;
  }

  return true;
}

bool check_if_present(
    const TypeSet& types,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework) {
  for (const DexType* type : types) {
    DexClass* cls = type_class(type);
    if (!cls || cls->is_external()) {
      // TODO(emmasevastian): When it isn't safe to continue here?
      continue;
    }

    if (!release_to_framework.count(type)) {
      return false;
    }
  }

  return true;
}

bool check_hierarchy(
    DexClass* cls,
    const api::FrameworkAPI& framework_api,
    const std::unordered_map<const DexType*, DexType*>& release_to_framework,
    const TypeSystem& type_system) {
  DexType* type = cls->get_type();
  if (!is_interface(cls)) {
    // We don't need to worry about subclasses, as those we just need to update
    // the superclass for.
    // TODO(emmasevastian): Any case when we should worry about subclasses?

    const auto& implemented_intfs =
        type_system.get_implemented_interfaces(type);
    if (!check_if_present(implemented_intfs, release_to_framework)) {
      return false;
    }

    auto* super_cls = cls->get_super_class();
    // We accept either Object or that the parent has an equivalent
    // framework class.
    // NOTE: That we would end up checking the parents up to chain when
    //       checking super_cls.
    // TODO(emmasevastian): If the parent is a framework class available on this
    //                      platform, we shouldn't fail.
    if (super_cls != known_types::java_lang_Object() &&
        release_to_framework.count(super_cls) == 0) {
      return false;
    }
  } else {
    TypeSet super_intfs;
    type_system.get_all_super_interfaces(type, super_intfs);

    if (!check_if_present(super_intfs, release_to_framework)) {
      return false;
    }
  }

  return true;
}

} // namespace

/**
 * Check that the replacements are valid:
 * - release library to framework classes have the same public members
 * - we have entire hierarchies (as in up the hierarchy, since subclasses
 *                               we can update)
 *
 * TODO(emmasevastian): Add extra checks: non public members? etc
 */
void ApiLevelsUtils::check_and_update_release_to_framework() {
  TypeSystem type_system(m_scope);

  // We need to check this in a loop, as an exclusion might have dependencies.
  while (true) {
    std::unordered_set<const DexType*> to_remove;

    // We need an up to date pairing from release library to framework classes,
    // for later use. So computing this on the fly, once.
    std::unordered_map<const DexType*, DexType*> release_to_framework;
    for (const auto& pair : m_types_to_framework_api) {
      release_to_framework[pair.first] = pair.second.cls;
    }

    for (const auto& pair : m_types_to_framework_api) {
      DexClass* cls = type_class(pair.first);
      always_assert(cls);

      if (!check_members(cls, pair.second, release_to_framework)) {
        to_remove.emplace(pair.first);
        continue;
      }

      if (!check_hierarchy(cls, pair.second, release_to_framework,
                           type_system)) {
        to_remove.emplace(pair.first);
      }
    }

    if (to_remove.empty()) {
      break;
    }

    for (const DexType* type : to_remove) {
      m_types_to_framework_api.erase(type);
    }
  }
}

/**
 * Loads information regarding support libraries / androidX etc to framework
 * APIs.
 */
void ApiLevelsUtils::load_types_to_framework_api() {
  std::unordered_map<DexType*, FrameworkAPI> framework_cls_to_api =
      get_framework_classes();
  std::unordered_map<std::string, DexType*> simple_cls_name_to_type =
      get_simple_cls_name_to_accepted_types(framework_cls_to_api);

  std::unordered_set<std::string> simple_names_releases;
  for (DexClass* cls : m_scope) {
    if (cls->is_external()) {
      continue;
    }

    const auto& cls_str = cls->get_deobfuscated_name();

    // TODO(emmasevastian): Better way of detecting release libraries ...
    if (boost::starts_with(cls_str, "Landroidx")) {
      std::string simple_name = get_simple_deobfuscated_name(cls->get_type());
      auto simple_cls_it = simple_cls_name_to_type.find(simple_name);
      if (simple_cls_it == simple_cls_name_to_type.end()) {
        continue;
      }

      // Assume there are no classes with the same simple name.
      // TODO(emmasevastian): Reconsider this! For now, leaving it as using
      //                      simple name, since paths have changed between
      //                      release and compatibility libraries.
      always_assert(simple_names_releases.count(simple_name) == 0);
      m_types_to_framework_api[cls->get_type()] =
          std::move(framework_cls_to_api[simple_cls_it->second]);
    }
  }

  // Checks and updates the mapping from release libraries to framework classes.
  check_and_update_release_to_framework();
}

void ApiLevelsUtils::filter_types(
    const std::unordered_set<const DexType*>& types) {
  for (const auto* type : types) {
    m_types_to_framework_api.erase(type);
  }

  // Make sure we clean up the dependencies.
  check_and_update_release_to_framework();
}

} // namespace api
