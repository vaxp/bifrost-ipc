# مرجع واجهة برمجة تطبيقات Bifrost (C API)

التوثيق المرجعي الشامل لكافة الأنواع والهياكل والدوال البرمجية في ترويسات `bifrost.h` و `bifrost_command.h`.

---

## 📋 رموز الأخطاء (`bifrost_error_t`)

```c
typedef enum {
    BIFROST_OK              =  0,  /**< نجاح العملية */
    BIFROST_ERR_NOMEM       = -1,  /**< نفاد الذاكرة */
    BIFROST_ERR_EXISTS      = -2,  /**< المجمع أو المشارك موجود مسبقاً */
    BIFROST_ERR_NOTFOUND    = -3,  /**< لم يتم العثور على المجمع أو المشارك أو الأمر */
    BIFROST_ERR_FULL        = -4,  /**< المجمع أو صندوق طابور الأوامر ممتلئ */
    BIFROST_ERR_INVALID     = -5,  /**< معطيات غير صالحة أو حجم حمولة غير مسموح */
    BIFROST_ERR_PERMISSION  = -6,  /**< تم رفض الصلاحية بواسطة مرشح الأمان */
    BIFROST_ERR_SYSTEM      = -7,  /**< خطأ نظام تشغيل داخلي */
    BIFROST_ERR_CORRUPTED   = -8,  /**< بيانات الذاكرة المشتركة تالفة */
    BIFROST_ERR_VERSION     = -9,  /**< عدم تطابق في إصدار البروتوكول */
    BIFROST_ERR_BUSY        = -10, /**< المورد مشغول (مثل طلب لم يكتمل تحريره) */
    BIFROST_ERR_TIMEOUT     = -11, /**< انتهت مهلة الانتظار المحددة */
    BIFROST_ERR_DISABLED    = -12  /**< الميزة معطلة في هذا المجمع */
} bifrost_error_t;
```

---

## 🏊 إدارة المجمع والمدير (Director API)

### `bifrost_config_init`
```c
void bifrost_config_init(bifrost_pool_config_t* config);
```
تهيئة هيكل الإعدادات `bifrost_pool_config_t` بالقيم الافتراضية المناسبة لبيئات الإنتاج.

### `bifrost_create_pool`
```c
bifrost_pool_t* bifrost_create_pool(const char* name, const bifrost_pool_config_t* config);
```
إنشاء مجمع ذاكرة مشتركة POSIX جديد تحت `/dev/shm/bifrost_<name>`.
*   `name`: اسم المجمع (بحد أقصى 56 حرفاً، وبدون علامة `/` في البداية).
*   `config`: هيكل الإعدادات، أو `NULL` لاعتماد القيم الافتراضية.
*   **القيمة المعادة**: مقبض المجمع عند النجاح، أو `NULL` عند الفشل.

### `bifrost_open_pool`
```c
bifrost_pool_t* bifrost_open_pool(const char* name);
```
فتح مجمع ذاكرة مشتركة موجود مسبقاً والارتباط به.

### `bifrost_close_pool`
```c
void bifrost_close_pool(bifrost_pool_t* pool);
```
إلغاء ربط المجمع بالعملية الحالية دون حذفه من النظام (`/dev/shm`).

### `bifrost_destroy_pool`
```c
void bifrost_destroy_pool(bifrost_pool_t* pool);
```
حذف ملف الذاكرة المشتركة نهائياً من النظام. يُستدعى بواسطة المدير عند إيقاف تشغيل الخدمة.

### `bifrost_get_pool_stats`
```c
bifrost_error_t bifrost_get_pool_stats(bifrost_pool_t* pool, bifrost_pool_stats_t* stats);
```
ملء هيكل `bifrost_pool_stats_t` بإحصائيات الاستخدام الحية (المساحة المستخدمة، الحرة، عدد المشاركين، عدادات القراءة والكتابة).

### `bifrost_expand_pool`
```c
bifrost_error_t bifrost_expand_pool(bifrost_pool_t* pool, size_t new_size);
```
زيادة السعة التخزينية لمجمع الذاكرة المشتركة.

### `bifrost_pool_exists`
```c
bool bifrost_pool_exists(const char* name);
```
فحص ما إذا كان مجمع الذاكرة المشتركة بالاسم المحدد موجوداً حالياً في النظام.

---

## ✍️ واجهة الكاتب وناشر البيانات (Writer API)

### `bifrost_join_as_writer`
```c
bifrost_writer_t* bifrost_join_as_writer(const char* pool_name, const char* writer_name);
```
تسجيل العملية ككاتب نشط في مجمع الذاكرة المحدد.
*   `writer_name`: معرف نصي فريد للكاتب (بحد أقصى 63 حرفاً).
*   **القيمة المعادة**: مقبض الكاتب أو `NULL` عند الفشل.

### `bifrost_writer_alloc`
```c
void* bifrost_writer_alloc(bifrost_writer_t* writer, size_t size);
```
حجز مساحة للبيانات داخل ساحة المجمع.
*   **القيمة المعادة**: مؤشر مباشر للذاكرة المشتركة دون نسخ.

### `bifrost_writer_publish`
```c
void bifrost_writer_publish(bifrost_writer_t* writer);
```
تأكيد نشر التعديلات باستخدام حواجز الذاكرة الذرية وزيادة رقم الإصدار التسلسلي.

### `bifrost_writer_set_command`
```c
void bifrost_writer_set_command(bifrost_writer_t* writer, uint32_t command);
```
تحديد وسم أو رمز العملية المصاحبة للبيانات ليطلع عليها القراء.

### `bifrost_writer_get_id`
```c
uint32_t bifrost_writer_get_id(bifrost_writer_t* writer);
```
الحصول على المعرف الرقمي الفريد المسند لخانات هذا الكاتب.

### `bifrost_writer_get_version`
```c
uint64_t bifrost_writer_get_version(bifrost_writer_t* writer);
```
الحصول على رقم الإصدار التسلسلي الحالي للبيانات المنشورة بواسطة الكاتب.

### `bifrost_writer_leave`
```c
void bifrost_writer_leave(bifrost_writer_t* writer);
```
تحرير خانات الكاتب وإلغاء تسجيله وإغلاق المقبض.

---

## 📖 واجهة القارئ ومستهلك البيانات (Reader API)

### `bifrost_join_as_reader`
```c
bifrost_reader_t* bifrost_join_as_reader(const char* pool_name, const char* reader_name);
```
تسجيل العملية الحالية كقارئ مشارك في مجمع الذاكرة.

### `bifrost_reader_get`
```c
void* bifrost_reader_get(bifrost_reader_t* reader, const char* writer_name, size_t* size_out);
```
الحصول على مؤشر مباشر لبيانات الكاتب دون أي عملية نسخ للذاكرة.

### `bifrost_reader_get_by_id`
```c
void* bifrost_reader_get_by_id(bifrost_reader_t* reader, uint32_t writer_id, size_t* size_out);
```
قراءة فورية وسريعة $O(1)$ باستخدام المعرف الرقمي للكاتب متجاوزاً مطابقة النصوص.

### `bifrost_reader_get_version`
```c
uint64_t bifrost_reader_get_version(bifrost_reader_t* reader, const char* writer_name);
```
قراءة العداد الذري لإصدار بيانات كاتب محدد لمعرفة ما إذا كانت البيانات قد حُدثت.

### `bifrost_reader_get_command` / `bifrost_reader_get_command_by_id`
```c
uint32_t bifrost_reader_get_command(bifrost_reader_t* reader, const char* writer_name);
uint32_t bifrost_reader_get_command_by_id(bifrost_reader_t* reader, uint32_t writer_id);
```
الحصول على وسم الأمر الذي وضعه الكاتب.

### `bifrost_reader_find_writer`
```c
uint32_t bifrost_reader_find_writer(bifrost_reader_t* reader, const char* writer_name);
```
البحث عن كاتب بالاسم والحصول على معرفه الرقمي (يعيد 0 إذا لم يتم العثور عليه).

### `bifrost_reader_list_writers`
```c
uint32_t bifrost_reader_list_writers(bifrost_reader_t* reader, bifrost_writer_info_t* info_out, uint32_t max_count);
```
جلب قائمة بكافة الكتاب النشطين حالياً في المجمع.

### `bifrost_reader_leave`
```c
void bifrost_reader_leave(bifrost_reader_t* reader);
```
إلغاء تسجيل القارئ وتحرير موارده.

---

## 🎛️ واجهة طبقة الأوامر والتحكم (`bifrost_command.h`)

### دوال الكاتب (Writer RPC)

*   `bifrost_error_t bifrost_writer_register_command(bifrost_writer_t* writer, const bifrost_cmd_desc_t* desc);`
    تسجيل أمر جديد في فهرس الكاتب المتاح للقراء.
*   `bifrost_error_t bifrost_writer_unregister_command(bifrost_writer_t* writer, uint32_t cmd_id);`
    إلغاء تسجيل أمر من الفهرس.
*   `const bifrost_cmd_request_t* bifrost_writer_poll_command(bifrost_writer_t* writer);`
    استطلاع الطلب التالي الوارد من القراء بشكل غير حاجب وبنسخ صفري مباشر.
*   `bifrost_error_t bifrost_writer_command_done(bifrost_writer_t* writer, const bifrost_cmd_request_t* req, int32_t status, const void* reply, uint32_t reply_len);`
    إكمال معالجة الطلب وإرسال حمولة الرد إلى صندوق القارئ.
*   `bifrost_error_t bifrost_writer_wait_command(bifrost_writer_t* writer, uint64_t timeout_ns);`
    انتظار وصول طلبات جديدة باستخدام futex لتوفير استهلاك المعالج.
*   `bifrost_error_t bifrost_writer_on_command(bifrost_writer_t* writer, uint32_t cmd_id, bifrost_cmd_handler_fn fn, void* ctx);`
    ربط دالة رد نداء تلقائية بأمر محدد.
*   `void bifrost_writer_set_command_filter(bifrost_writer_t* writer, bifrost_cmd_filter_fn fn, void* ctx);`
    تثبيت دالة للتحقق من صلاحيات الطلبات قبل تمريرها لمعالجات الأوامر.
*   `uint32_t bifrost_writer_dispatch(bifrost_writer_t* writer, uint32_t max_cmds);`
    معالجة وتوزيع ما يصل إلى `max_cmds` من الطلبات المعلقة تلقائياً عبر المعالجات المسجلة.
*   `bifrost_error_t bifrost_writer_get_sender_name(bifrost_writer_t* writer, const bifrost_cmd_request_t* req, char* out, size_t out_size);`
    جلب اسم القارئ المرسل للطلب لأغراض التسجيل والتدقيق.
*   `uint64_t bifrost_writer_replies_dropped(bifrost_writer_t* writer);`
    عدد الردود التي تم إسقاطها بسبب امتلاء صندوق القارئ أو إغلاق عمليته.

### دوال القارئ (Reader RPC)

*   `bifrost_writer_ref_t bifrost_reader_resolve_writer(bifrost_reader_t* reader, const char* writer_name);`
    الحصول على مرجع آمن للكاتب محمي ضد مشاكل ABA (`id` + `gen`).
*   `bool bifrost_reader_writer_alive(bifrost_reader_t* reader, bifrost_writer_ref_t writer);`
    التحقق من أن الكاتب المستهدف لا يزال حياً ولم تتم إعادة تدوير خانته.
*   `uint32_t bifrost_reader_list_commands(bifrost_reader_t* reader, bifrost_writer_ref_t writer, bifrost_cmd_info_t* out, uint32_t max_count);`
    استعراض فهرس الأوامر التي يدعمها الكاتب المحدد.
*   `uint32_t bifrost_reader_find_command(bifrost_reader_t* reader, bifrost_writer_ref_t writer, const char* cmd_name, bifrost_cmd_info_t* info_out);`
    البحث عن معرف أمر بالاسم داخل فهرس الكاتب.
*   `bifrost_error_t bifrost_reader_send_command(bifrost_reader_t* reader, bifrost_writer_ref_t writer, uint32_t cmd_id, const void* args, uint32_t arg_len, uint32_t flags, uint64_t* ticket_out);`
    إرسال طلب أمر غير متزامن إلى صندوق الكاتب.
*   `const bifrost_cmd_reply_t* bifrost_reader_poll_reply(bifrost_reader_t* reader);`
    استطلاع الردود الواردة من الكتاب.
*   `void bifrost_reader_reply_done(bifrost_reader_t* reader, const bifrost_cmd_reply_t* reply);`
    تحرير خانة الرد بعد الانتهاء من قراءته.
*   `bifrost_error_t bifrost_reader_wait_reply(bifrost_reader_t* reader, uint64_t timeout_ns);`
    انتظار وصول رد باستخدام futex.
*   `bifrost_error_t bifrost_reader_call(bifrost_reader_t* reader, bifrost_writer_ref_t writer, uint32_t cmd_id, const void* args, uint32_t arg_len, void* reply_buf, uint32_t* reply_len, int32_t* status_out, uint64_t timeout_ns);`
    إجراء استدعاء إجرائي متزامن (RPC): إرسال الطلب وانتظار الرد المقابل له مباشرة.

---

## 🛠️ دوال الخدمات والمرافق

*   `bifrost_error_t bifrost_get_last_error(void);`
    الحصول على رمز الخطأ الأخير للمسلك الحالي.
*   `const char* bifrost_error_string(bifrost_error_t error);`
    الحصول على النص التوضيحي المقابل لرمز الخطأ.
*   `const char* bifrost_version(void);`
    الحصول على نص إصدار المكتبة (مثال: `"0.1.0"`).
