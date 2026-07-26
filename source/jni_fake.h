/* jni_fake.h -- fake JNI environment for libemucore.so
 *
 * The core talks to a Java surface we replace natively: FileHelper (file IO),
 * PreferenceHelpers/SharedPreferences (settings), NativeLibrary static callbacks
 * (OSD/vibration/asset reads), and a set of result classes. The fake JNIEnv
 * dispatches Call*Method by interned (name,sig); Get*Field reads typed fields off
 * the fake objects built below.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

// The fake JNIEnv / JavaVM handed to the core. Both are pointer-to-pointer-to-
// function-table, matching the JNI ABI the core calls through.
extern void *fake_env;
extern void *fake_vm;

void jni_init(void);

// ---- fake object builders (used by filehelper.c / main.c to hand typed Java
// objects back to the core; the env's Get*Field reads these by field name) ----

void *jni_make_string(const char *utf);           // -> fake jstring
const char *jni_string_utf(void *jstr);           // fake jstring -> utf8 (or NULL)

void *jni_obj_new(const char *clsname);           // empty fake object tagged with class
void  jni_obj_set_long  (void *obj, const char *field, int64_t v);
void  jni_obj_set_int   (void *obj, const char *field, int32_t v);
int32_t jni_obj_get_int (void *obj, const char *field, int32_t def);
int64_t jni_obj_get_long(void *obj, const char *field, int64_t def);
const char *jni_obj_get_string(void *obj, const char *field, const char *def);
void  jni_obj_set_float (void *obj, const char *field, float v);
void  jni_obj_set_bool  (void *obj, const char *field, int v);
void  jni_obj_set_string(void *obj, const char *field, const char *utf);
void  jni_obj_set_object(void *obj, const char *field, void *child);

void *jni_make_byte_array(const void *data, int len);   // -> fake jbyteArray
void *jni_make_object_array(int n);                     // n NULL slots
void  jni_obj_array_set(void *arr, int i, void *elem);
int   jni_obj_array_len(void *arr);
void *jni_obj_array_get(void *arr, int i);

#endif
