# دليل المستخدم الشامل لمكتبة Bifrost

يشرح هذا الدليل كافة الجوانب التشغيلية لمكتبة Bifrost: بنية الذاكرة المشتركة، تدفق البيانات بنسخ صفري (Zero-Copy)، طبقة الأوامر والتحكم (RPC) الخالية تماماً من الأقفال (Lock-Free)، وبوابة الشبكة عالية السرعة.

---

## 🏗️ البنية المعمارية والمفاهيم الأساسية

```
┌─────────────────────────────────────────────────────────────┐
│                 المدير (منشئ مجمع الذاكرة)                  │
│               bifrost_create_pool("my_pool")                │
└──────────────────────────────┬──────────────────────────────┘
                               │ ينشئ /dev/shm/bifrost_my_pool
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                     مجمع الذاكرة المشتركة                   │
│ ┌───────────────┬────────────────────────┬────────────────┐ │
│ │ترويسة المجمع  │ مصفوفة المشاركين       │ ساحة البيانات  │ │
│ │ (Stats, Magic)│ (أجيال ABA والخانات)   │(Bump Allocator)│ │
│ ├───────────────┴────────────────────────┴────────────────┤ │
│ │     منطقة الأوامر (حلقات صناديق الطلبات والردود الموزعة) │ │
│ └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────┬──────────────────────────────┘
         ▲                     │                     ▲
         │ مؤشرات ذاكرة مباشرة │ مؤشرات ذاكرة مباشرة │
┌────────┴────────┐            │            ┌────────┴────────┐
│  الكاتب (خادم)  │◀───────────┴───────────▶│  القارئ (واجهة) │
│ (خدمة النظام)   │        طلبات الأوامر    │(مستهلك البيانات)│
└─────────────────┘        والردود الذرية   └─────────────────┘
```

1. **المجمع (Pool)**: كائن ذاكرة مشتركة POSIX في مسار `/dev/shm/bifrost_NAME`. يحتوي على ترويسة النظام، ومعلومات المشاركين، وساحة البيانات، وحلقات طوابير الأوامر.
2. **المدير (Director)**: العملية المسؤولة عن إنشاء وتهيئة المجمع لأول مرة عبر `bifrost_create_pool()`.
3. **الكاتب (Writer)**: عملية تنشر البيانات في خاناتها المخصصة وتستقبل أوامر التحكم من القراء.
4. **القارئ (Reader)**: عملية تكتشف الكتاب، وتقرأ البيانات مباشرة من الذاكرة المشتركة دون نسخ (`memcpy`)، وترسل أوامر RPC.
5. **منطقة الأوامر (Command Region)**: طوابير حلقية MPSC محصورة وعالية السرعة تضمن إرسال واستقبال الطلبات والردود ذرياً بدون أي أقفال (Mutexes).

---

## 📝 خطوات الاستخدام والتطبيق

### المرحلة 1: إنشاء وتهيئة مجمع الذاكرة

يقوم خادم النظام أو مدير العمليات بتهيئة وإنشاء المجمع عند بدء التشغيل:

```c
#include <stdio.h>
#include <bifrost.h>

int main(void) {
    bifrost_pool_config_t config;
    bifrost_config_init(&config);

    // إعدادات ساحة البيانات
    config.initial_size = 16 * 1024 * 1024;    // 16 ميغابايت حجم مبدئي
    config.max_size = 256 * 1024 * 1024;       // 256 ميغابايت كحد أقصى للتوسع
    config.max_participants = 64;              // أقصى عدد للمشاركين (كتاب + قراء)
    config.max_slots_per_writer = 16;          // أقصى عدد خانات للكاتب الواحد
    config.auto_expand = true;                 // توسع تلقائي عند الحاجة

    // إعدادات طبقة الأوامر (Command Layer)
    config.cmd_inbox_depth = 64;               // عمق صندوق طلبات الكاتب (مضاعف للرقم 2)
    config.cmd_reply_depth = 32;               // عمق صندوق ردود القارئ
    config.cmd_max_args = 256;                 // أقصى حجم لمعطيات الطلب بالبايت
    config.cmd_max_reply = 256;                // أقصى حجم لمعطيات الرد بالبايت
    config.cmd_max_per_writer = 32;            // سعة فهرس الأوامر لكل كاتب

    bifrost_pool_t* pool = bifrost_create_pool("production_pool", &config);
    if (!pool) {
        // في حال كان المجمع موجوداً من تشغيل سابق نكتفي بفتحه
        pool = bifrost_open_pool("production_pool");
        if (!pool) {
            fprintf(stderr, "فشل إنشاء أو فتح مجمع الذاكرة المشتركة\n");
            return 1;
        }
    }

    printf("تمت تهيئة مجمع الذاكرة المشتركة بنجاح.\n");
    return 0;
}
```

---

### المرحلة 2: الكاتب (نشر البيانات)

يقوم الكاتب بحجز الذاكرة مباشرة داخل المجمع والتعديل عليها فورياً:

```c
#include <bifrost.h>
#include <unistd.h>

typedef struct {
    double cpu_usage;
    uint64_t ram_bytes_used;
    uint32_t active_tasks;
} SystemMetrics;

int main(void) {
    // 1. الانضمام ككاتب
    bifrost_writer_t* writer = bifrost_join_as_writer("production_pool", "sysmon_service");
    if (!writer) return 1;

    // 2. الحجز مرة واحدة (تجنب إعادة الحجز المتكرر في الحلقات السريعة)
    SystemMetrics* metrics = (SystemMetrics*)bifrost_writer_alloc(writer, sizeof(SystemMetrics));

    while (running) {
        // 3. كتابة وتعديل البيانات مباشرة في الذاكرة المشتركة
        metrics->cpu_usage = read_cpu();
        metrics->ram_bytes_used = read_ram();
        metrics->active_tasks = read_tasks();

        // 4. نشر التحديث (يزيد رقم الإصدار الذري فوراً)
        bifrost_writer_publish(writer);

        usleep(50000); // تحديث كل 50 مللي ثانية
    }

    // 5. الخروج النظيف
    bifrost_writer_leave(writer);
    return 0;
}
```

---

### المرحلة 3: القارئ (استهلاك البيانات بنسخ صفري)

يمكن للقارئ القراءة السريعة إما عبر الاسم أو عبر المعرف المخزن مؤقتاً (Cached ID):

```c
#include <stdio.h>
#include <unistd.h>
#include <bifrost.h>

int main(void) {
    bifrost_reader_t* reader = bifrost_join_as_reader("production_pool", "status_widget");
    if (!reader) return 1;

    // جلب معرف الكاتب لتسريع القراءة وتفادي مقارنة النصوص في كل دورة
    uint32_t writer_id = 0;
    while (!writer_id && running) {
        writer_id = bifrost_reader_find_writer(reader, "sysmon_service");
        if (!writer_id) usleep(10000);
    }

    uint64_t last_version = 0;

    while (running) {
        // قراءة مباشرة وفورية باستخدام المعرف
        size_t size = 0;
        const SystemMetrics* metrics = (const SystemMetrics*)bifrost_reader_get_by_id(
            reader, writer_id, &size
        );

        if (metrics) {
            uint64_t ver = bifrost_reader_get_version(reader, "sysmon_service");
            if (ver > last_version) {
                printf("المعالج: %.1f%% | الذاكرة: %lu ميغابايت\n", 
                       metrics->cpu_usage, metrics->ram_bytes_used / (1024 * 1024));
                last_version = ver;
            }
        }
        usleep(50000);
    }

    bifrost_reader_leave(reader);
    return 0;
}
```

---

### المرحلة 4: طبقة الأوامر والتحكم (Lock-Free RPC)

تتيح طبقة الأوامر للقراء استدعاء إجراءات ينفذها الكاتب مع تمرير معطيات واستقبال ردود دون إيقاف الكاتب.

#### الطريقة الأولى: استطلاع الأوامر يدوياً والرد المباشر

```c
#include <bifrost.h>
#include <bifrost_command.h>

#define CMD_SET_POWER_MODE 10

// 1. تسجيل الأمر في فهرس الكاتب
bifrost_cmd_desc_t desc = {
    .id = CMD_SET_POWER_MODE,
    .name = "set_power_mode",
    .desc = "Sets power profile: 0=low, 1=balanced, 2=performance",
    .arg_min = sizeof(int32_t),
    .arg_max = sizeof(int32_t),
    .reply_max = sizeof(int32_t)
};
bifrost_writer_register_command(writer, &desc);

// 2. فحص صندوق الطلبات داخل حلقة المعالجة
const bifrost_cmd_request_t* req = bifrost_writer_poll_command(writer);
if (req) {
    if (req->cmd_id == CMD_SET_POWER_MODE) {
        int32_t new_mode = *(const int32_t*)req->args;
        int32_t result = apply_power_mode(new_mode);

        // إرسال الرد مباشرة لطالب الأمر
        bifrost_writer_command_done(writer, req, BIFROST_OK, &result, sizeof(result));
    }
}
```

#### الطريقة الثانية: التوزيع التلقائي (Dispatcher Callbacks)

```c
static int32_t handle_power_mode(const bifrost_cmd_request_t* req, 
                                 void* reply, uint32_t* reply_len, void* ctx) {
    int32_t mode = *(const int32_t*)req->args;
    *(int32_t*)reply = apply_power_mode(mode);
    *reply_len = sizeof(int32_t);
    return BIFROST_OK;
}

// ربط المعالج بالأمر
bifrost_writer_on_command(writer, CMD_SET_POWER_MODE, handle_power_mode, NULL);

// في حلقة الأحداث، معالجة الطلبات المتراكمة تلقائياً (حتى 8 طلبات في الدورة الواحدة):
bifrost_writer_dispatch(writer, 8);
```

#### استدعاء الأمر من طرف القارئ (RPC متزامن)

```c
bifrost_writer_ref_t target = bifrost_reader_resolve_writer(reader, "sysmon_service");
if (target.id != 0) {
    int32_t desired_mode = 2; // وضع الأداء الأقصى
    int32_t reply_val = 0;
    uint32_t reply_len = sizeof(reply_val);
    int32_t status = 0;

    // إرسال وانتظار الرد بمهلة 100 مللي ثانية
    bifrost_error_t err = bifrost_reader_call(
        reader, target,
        CMD_SET_POWER_MODE,
        &desired_mode, sizeof(desired_mode),
        &reply_val, &reply_len,
        &status,
        100000000ULL // 100ms بالنانوثانية
    );

    if (err == BIFROST_OK && status == BIFROST_OK) {
        printf("تم تغيير نمط الطاقة بنجاح. النتيجة: %d\n", reply_val);
    }
}
```

---

## ⚡ قواعد التزامن وأفضل الممارسات

1. **تزامن المسالك (Threading)**:
   - يجب أن يُستخدم مقبض `bifrost_writer_t` أو `bifrost_reader_t` من قِبل **مسلك واحد فقط** (Single Thread) في الوقت نفسه.
   - إذا كان لديك مسالك متعددة ترغب في الإرسال بالتوازي، أنشئ مقبضاً مستقلاً لكل مسلك.
2. **الحجز في المسارات الساخنة (Hot Paths)**:
   - احجز الذاكرة دائماً عبر `bifrost_writer_alloc()` مرة واحدة عند بدء التشغيل، وحدث الحقول مباشرة داخل المؤشر.
3. **الحماية من مشاكل إعادة الاستخدام (ABA Protection)**:
   - استخدم دائماً `bifrost_reader_resolve_writer()` للحصول على `bifrost_writer_ref_t`.
   - يحفظ حقل الجيل (Generation) سلامة النظام؛ فإذا انهارت عملية كاتب وحلت مكانها عملية أخرى في نفس الخانة، تفشل الطلبات القديمة بأمان بدلاً من إرسالها للعملية الخاطئة.
