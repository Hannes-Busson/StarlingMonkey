#include "msgpack.h"
#include "builtin.h"

#include "js/ArrayBuffer.h"
#include "js/experimental/TypedData.h"

#include "mpack.h"

namespace builtins::web::msgpack {

// --- encode (JS -> msgpack) ---

static bool encode_value(mpack_writer_t *writer, JSContext *cx, JS::HandleValue val) {
  if (val.isNull() || val.isUndefined()) {
    mpack_write_nil(writer);
    return true;
  }
  if (val.isBoolean()) {
    mpack_write_bool(writer, val.toBoolean());
    return true;
  }
  if (val.isInt32()) {
    mpack_write_int(writer, val.toInt32());
    return true;
  }
  if (val.isDouble()) {
    mpack_write_double(writer, val.toDouble());
    return true;
  }
  if (val.isString()) {
    JS::RootedString str(cx, val.toString());
    JS::UniqueChars bytes = JS_EncodeStringToUTF8(cx, str);
    if (!bytes) return false;
    size_t len = strlen(bytes.get());
    mpack_write_str(writer, bytes.get(), (uint32_t)len);
    return true;
  }
  if (val.isObject()) {
    JS::RootedObject obj(cx, &val.toObject());

    // Uint8Array → bin
    {
      uint8_t *data = nullptr;
      bool shared = false;
      size_t len = 0;
      if (JS_GetObjectAsUint8Array(obj, &len, &shared, &data)) {
        mpack_write_bin(writer, (const char *)data, (uint32_t)len);
        return true;
      }
    }

    // Array
    bool is_array = false;
    if (!JS::IsArrayObject(cx, obj, &is_array)) return false;
    if (is_array) {
      uint32_t length = 0;
      if (!JS::GetArrayLength(cx, obj, &length)) return false;
      mpack_start_array(writer, length);
      JS::RootedValue elem(cx);
      for (uint32_t i = 0; i < length; i++) {
        if (!JS_GetElement(cx, obj, i, &elem)) return false;
        if (!encode_value(writer, cx, elem)) return false;
      }
      mpack_finish_array(writer);
      return true;
    }

    // Plain object → map
    JS::RootedIdVector ids(cx);
    if (!js::GetPropertyKeys(cx, obj, JSITER_OWNONLY, &ids)) return false;
    mpack_start_map(writer, (uint32_t)ids.length());
    JS::RootedValue prop_val(cx);
    for (size_t i = 0; i < ids.length(); i++) {
      JS::RootedId id(cx, ids[i]);
      // write key as string
      JS::RootedValue id_val(cx);
      if (!JS_IdToValue(cx, id, &id_val)) return false;
      JS::RootedString key_str(cx, JS::ToString(cx, id_val));
      if (!key_str) return false;
      JS::UniqueChars key_bytes = JS_EncodeStringToUTF8(cx, key_str);
      if (!key_bytes) return false;
      size_t key_len = strlen(key_bytes.get());
      mpack_write_str(writer, key_bytes.get(), (uint32_t)key_len);
      // write value
      if (!JS_GetPropertyById(cx, obj, id, &prop_val)) return false;
      if (!encode_value(writer, cx, prop_val)) return false;
    }
    mpack_finish_map(writer);
    return true;
  }

  // fallback: write nil
  mpack_write_nil(writer);
  return true;
}

static bool msgpackEncode(JSContext *cx, unsigned argc, JS::Value *vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "msgpackEncode", 1)) return false;

  mpack_writer_t writer;
  char *buf = nullptr;
  size_t size = 0;
  mpack_writer_init_growable(&writer, &buf, &size);

  JS::RootedValue input(cx, args.get(0));
  if (!encode_value(&writer, cx, input)) {
    mpack_writer_destroy(&writer);
    free(buf);
    return false;
  }

  if (mpack_writer_destroy(&writer) != mpack_ok) {
    free(buf);
    JS_ReportErrorASCII(cx, "msgpackEncode: serialization error");
    return false;
  }

  // Hand ownership of buf to the ArrayBuffer
  JS::RootedObject buffer(
      cx, JS::NewArrayBufferWithContents(cx, size, buf,
                                         JS::NewArrayBufferOutOfMemory::CallerMustFreeMemory));
  if (!buffer) {
    free(buf);
    return false;
  }
  // buffer now owns buf — do NOT free it

  JS::RootedObject byte_array(cx, JS_NewUint8ArrayWithBuffer(cx, buffer, 0, size));
  if (!byte_array) return false;

  args.rval().setObject(*byte_array);
  return true;
}

// --- decode (msgpack -> JS) ---

static bool decode_node(mpack_node_t node, JSContext *cx, JS::MutableHandleValue out) {
  mpack_type_t type = mpack_node_type(node);
  switch (type) {
  case mpack_type_nil:
    out.setNull();
    return true;
  case mpack_type_bool:
    out.setBoolean(mpack_node_bool(node));
    return true;
  case mpack_type_int:
    out.setNumber((double)mpack_node_int(node));
    return true;
  case mpack_type_uint:
    out.setNumber((double)mpack_node_uint(node));
    return true;
  case mpack_type_float:
    out.setNumber((double)mpack_node_float(node));
    return true;
  case mpack_type_double:
    out.setNumber(mpack_node_double(node));
    return true;
  case mpack_type_str: {
    size_t len = mpack_node_strlen(node);
    const char *data = mpack_node_str(node);
    JS::RootedString str(cx, JS_NewStringCopyN(cx, data, len));
    if (!str) return false;
    out.setString(str);
    return true;
  }
  case mpack_type_bin: {
    size_t len = mpack_node_bin_size(node);
    const char *data = mpack_node_bin_data(node);
    // copy into a new Uint8Array
    JS::RootedObject buf(cx, JS::NewArrayBuffer(cx, len));
    if (!buf) return false;
    {
      bool shared = false;
      JS::AutoCheckCannotGC nogc;
      uint8_t *ptr = JS::GetArrayBufferData(buf, &shared, nogc);
      memcpy(ptr, data, len);
    }
    JS::RootedObject arr(cx, JS_NewUint8ArrayWithBuffer(cx, buf, 0, len));
    if (!arr) return false;
    out.setObject(*arr);
    return true;
  }
  case mpack_type_array: {
    uint32_t count = (uint32_t)mpack_node_array_length(node);
    JS::RootedObject arr(cx, JS::NewArrayObject(cx, count));
    if (!arr) return false;
    JS::RootedValue elem(cx);
    for (uint32_t i = 0; i < count; i++) {
      if (!decode_node(mpack_node_array_at(node, i), cx, &elem)) return false;
      if (!JS_SetElement(cx, arr, i, elem)) return false;
    }
    out.setObject(*arr);
    return true;
  }
  case mpack_type_map: {
    uint32_t count = (uint32_t)mpack_node_map_count(node);
    JS::RootedObject obj(cx, JS_NewPlainObject(cx));
    if (!obj) return false;
    JS::RootedValue val(cx);
    for (uint32_t i = 0; i < count; i++) {
      mpack_node_t key_node = mpack_node_map_key_at(node, i);
      mpack_node_t val_node = mpack_node_map_value_at(node, i);
      size_t key_len = mpack_node_strlen(key_node);
      const char *key_data = mpack_node_str(key_node);
      if (!decode_node(val_node, cx, &val)) return false;
      // JS_SetProperty needs a null-terminated key; copy it
      JS::RootedString key_str(cx, JS_NewStringCopyN(cx, key_data, key_len));
      if (!key_str) return false;
      JS::RootedId key_id(cx);
      if (!JS_StringToId(cx, key_str, &key_id)) return false;
      if (!JS_DefinePropertyById(cx, obj, key_id, val, JSPROP_ENUMERATE)) return false;
    }
    out.setObject(*obj);
    return true;
  }
  default:
    out.setNull();
    return true;
  }
}

static bool msgpackDecode(JSContext *cx, unsigned argc, JS::Value *vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "msgpackDecode", 1)) return false;

  JS::RootedValue input(cx, args.get(0));
  if (!input.isObject()) {
    JS_ReportErrorASCII(cx, "msgpackDecode: expected Uint8Array");
    return false;
  }
  JS::RootedObject input_obj(cx, &input.toObject());

  uint8_t *data = nullptr;
  bool shared = false;
  size_t len = 0;
  if (!JS_GetObjectAsUint8Array(input_obj, &len, &shared, &data)) {
    JS_ReportErrorASCII(cx, "msgpackDecode: expected Uint8Array");
    return false;
  }

  mpack_tree_t tree;
  mpack_tree_init_data(&tree, (const char *)data, len);
  mpack_tree_parse(&tree);

  if (mpack_tree_error(&tree) != mpack_ok) {
    mpack_tree_destroy(&tree);
    JS_ReportErrorASCII(cx, "msgpackDecode: invalid msgpack data");
    return false;
  }

  mpack_node_t root = mpack_tree_root(&tree);
  JS::RootedValue result(cx);
  bool ok = decode_node(root, cx, &result);

  mpack_tree_destroy(&tree);
  if (!ok) return false;

  args.rval().set(result);
  return true;
}

static const JSFunctionSpec methods[] = {
    JS_FN("msgpackEncode", msgpackEncode, 1, JSPROP_ENUMERATE),
    JS_FN("msgpackDecode", msgpackDecode, 1, JSPROP_ENUMERATE),
    JS_FS_END,
};

bool install(api::Engine *engine) {
  return JS_DefineFunctions(engine->cx(), engine->global(), methods);
}

} // namespace builtins::web::msgpack
