# دليل البدء السريع مع Bifrost

مكتبة **Bifrost** هي مكتبة اتصال بين العمليات (IPC) فائقة السرعة وخالية تماماً من الأقفال (Lock-Free)، مبنية بالاعتماد على عمليات C11 الذرية (Atomics) والذاكرة المشتركة لنظام لينكس (`/dev/shm`). تم تصميمها لتحقيق أزمنة استجابة تقاس بالميكروثانية ونقل مباشر للبيانات بدون نسخ (Zero-Copy)، بالإضافة إلى نظام أوامر وطلبات (RPC) غير حاجب.

---

## 📦 المتطلبات وحزم النظام

*   **نظام التشغيل**: لينكس (Linux Kernel 3.17+ مع دعم كامل لـ memfd و futex، ويوصى بـ 5.4+ لدعم شبكات AF_XDP)
*   **المترجم**: GCC 4.9+ أو Clang 3.6+ (مع دعم لمعيار C11)
*   **نظام البناء**: [Meson](https://mesonbuild.com/) (0.55 أو أحدث) مع [Ninja](https://ninja-build.org/)
*   **الاعتماديات القياسية**:
    *   `librt` و `pthreads` (مكتبات POSIX قياسية مدمجة في نظام لينكس)
    *   `libbpf-dev` و `libxdp-dev` *(اختياري، لتشغيل ميزات تسريع الشبكات وتجاوز نواة النظام AF_XDP)*

### تثبيت الحزم المطلوبة

على توزيعات **Ubuntu / Debian**:
```bash
sudo apt update
sudo apt install -y build-essential meson ninja-build libbpf-dev libxdp-dev
```

على توزيعات **Fedora / RHEL / CentOS**:
```bash
sudo dnf install -y gcc meson ninja-build libbpf-devel libxdp-devel
```

على توزيعات **Arch Linux**:
```bash
sudo pacman -S --needed base-devel meson ninja libbpf
```

---

## 🛠️ البناء والتثبيت

تعتمد Bifrost بشكل أساسي على **Meson** و **Ninja** كنظام بناء رسمي.

### 1. التهيئة والبناء

```bash
# تهيئة مجلد البناء بنمط الإصدار (Release) مع تفعيل تحسينات المعالج المتقدمة
meson setup build --prefix=/usr --buildtype=release

# تصريف المكتبة، والأمثلة، وحزم الاختبارات
ninja -C build
```

### 2. تشغيل الاختبارات وقياس الأداء (Benchmarks)

```bash
# تشغيل مجموعة الاختبارات الآلية
meson test -C build

# أو تشغيل اختبار الأداء الفائق المباشر
./build/tests/bifrost_ultra_bench
```

### 3. التثبيت على مستوى النظام (اختياري)

```bash
sudo ninja -C build install
sudo ldconfig
```

يقوم أمر التثبيت بوضع:
- ملفات الترويسة في `/usr/include/bifrost.h` و `/usr/include/bifrost_command.h`
- المكتبة الديناميكية في `/usr/lib/libbifrost.so`
- المكتبة الساكنة في `/usr/lib/libbifrost_static.a`

---

## 🔗 ربط المكتبة بمشروعك الخاص

### الخيار الأول: باستخدام نظام Meson (الموصى به)
إذا كان مشروعك يستخدم Meson، يمكنك استدعاء Bifrost مباشرة عبر `dependency('bifrost')`:

```meson
project('my_project', 'c', default_options : ['c_std=gnu11'])

bifrost_dep = dependency('bifrost', required : true)
# أو في حال الربط اليدوي:
# cc = meson.get_compiler('c')
# bifrost_dep = cc.find_library('bifrost', required : true)
# rt_dep = cc.find_library('rt', required : true)

executable('my_app', 'main.c', dependencies : [bifrost_dep])
```

### الخيار الثاني: باستخدام CMake
إذا كان مشروعك يستخدم CMake، يمكنك إيجاد المكتبة عبر `find_library` أو `pkg-config`:

```cmake
cmake_minimum_required(VERSION 3.12)
project(my_project C)

set(CMAKE_C_STANDARD 11)

find_library(BIFROST_LIB bifrost REQUIRED)
find_package(Threads REQUIRED)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE ${BIFROST_LIB} Threads::Threads rt)
```

### الخيار الثالث: التصريف المباشر عبر GCC أو Clang

```bash
gcc -std=c11 -O3 main.c -o my_app -lbifrost -lpthread -lrt
```

---

## 🚀 أمثلة البدء السريع

### مثال 1: عملية النشر والكتابة (Writer / Server)
خدمة خلفية (Daemon) تنشر قياسات الحساسات عبر الذاكرة المشتركة دون أي أقفال أو عمليات نسخ للذاكرة:

```c
#include <stdio.h>
#include <unistd.h>
#include <bifrost.h>

typedef struct {
    double cpu_temp;
    uint64_t frame_index;
} TelemetryData;

int main(void) {
    // 1. تهيئة الإعدادات وإنشاء مجمع الذاكرة المشتركة
    bifrost_pool_config_t config;
    bifrost_config_init(&config);
    config.initial_size = 4 * 1024 * 1024; // 4 ميغابايت

    bifrost_pool_t* pool = bifrost_create_pool("system_telemetry", &config);
    if (!pool) {
        // في حال كان المجمع منشأ مسبقاً، نكتفي بفتحه
        pool = bifrost_open_pool("system_telemetry");
    }

    // 2. الانضمام إلى المجمع ككاتب
    bifrost_writer_t* writer = bifrost_join_as_writer("system_telemetry", "hwmon_daemon");

    // 3. حجز مساحة في الذاكرة المشتركة (مؤشر مباشر بنسخ صفري)
    TelemetryData* data = (TelemetryData*)bifrost_writer_alloc(writer, sizeof(TelemetryData));

    // 4. تحديث البيانات ونشرها للمشتركين
    for (int i = 0; i < 100; i++) {
        data->cpu_temp = 42.5 + (i * 0.1);
        data->frame_index = i;

        // نشر ذري: يظهر فوراً لجميع القراء
        bifrost_writer_publish(writer);
        usleep(10000); // 10 مللي ثانية
    }

    // 5. التنظيف عند الانتهاء
    bifrost_writer_leave(writer);
    bifrost_close_pool(pool);
    return 0;
}
```

### مثال 2: عملية الاستهلاك والقراءة (Reader / Client)
واجهة مستخدم أو عملية قارئة تقرأ البيانات المنشورة مباشرة من الذاكرة المشتركة:

```c
#include <stdio.h>
#include <unistd.h>
#include <bifrost.h>

typedef struct {
    double cpu_temp;
    uint64_t frame_index;
} TelemetryData;

int main(void) {
    // 1. الانضمام إلى المجمع كقارئ
    bifrost_reader_t* reader = bifrost_join_as_reader("system_telemetry", "status_ui");
    if (!reader) {
        fprintf(stderr, "تعذر الاتصال بمجمع الذاكرة!\n");
        return 1;
    }

    uint64_t last_ver = 0;

    // 2. استطلاع التحديثات
    for (int i = 0; i < 50; i++) {
        uint64_t ver = bifrost_reader_get_version(reader, "hwmon_daemon");
        if (ver > last_ver) {
            size_t size = 0;
            // قراءة فورية عبر مؤشر مباشر (Zero-Copy بدون memcpy!)
            const TelemetryData* data = (const TelemetryData*)bifrost_reader_get(reader, "hwmon_daemon", &size);
            if (data) {
                printf("[واجهة المستخدم] إطار #%lu | حرارة المعالج: %.1f س\n", data->frame_index, data->cpu_temp);
                last_ver = ver;
            }
        }
        usleep(10000);
    }

    // 3. التنظيف
    bifrost_reader_leave(reader);
    return 0;
}
```

### مثال 3: طبقة الأوامر والتحكم (Lock-Free RPC)
توفر Bifrost عبر الترويسة `<bifrost_command.h>` نظام استدعاءات إجرائية (RPC) فائق السرعة وخالٍ من التعليق:

**طرف الكاتب (الخادم - Server):**
```c
#include <bifrost.h>
#include <bifrost_command.h>

// تسجيل الأمر في الفهرس
bifrost_cmd_desc_t desc = {
    .id = 101,
    .name = "ping",
    .desc = "طلب فحص الاتصال",
    .arg_min = 0,
    .arg_max = 64,
    .reply_max = 64
};
bifrost_writer_register_command(writer, &desc);

// داخل حلقة الأحداث:
const bifrost_cmd_request_t* req = bifrost_writer_poll_command(writer);
if (req) {
    // معالجة الطلب وإرسال الرد
    const char reply_msg[] = "pong";
    bifrost_writer_command_done(writer, req, BIFROST_OK, reply_msg, sizeof(reply_msg));
}
```

**طرف القارئ (العميل - Client):**
```c
#include <bifrost.h>
#include <bifrost_command.h>

bifrost_writer_ref_t server = bifrost_reader_resolve_writer(reader, "hwmon_daemon");
if (server.id != 0) {
    char reply[64] = {0};
    uint32_t reply_len = sizeof(reply);
    int32_t status = 0;

    // استدعاء متزامن مع مهلة انتظار 500 مللي ثانية
    bifrost_error_t err = bifrost_reader_call(
        reader, server, 101, NULL, 0,
        reply, &reply_len, &status, 500000000ULL
    );

    if (err == BIFROST_OK && status == BIFROST_OK) {
        printf("تم استقبال الرد: %s\n", reply);
    }
}
```

---

## 📖 الأدلة التفصيلية التالية

*   [**دليل المستخدم الشامل (User Guide)**](02_user_guide.md): شرح إدارة المجمعات والمشاركين وطبقة الأوامر والتوسع الديناميكي.
*   [**مرجع واجهة البرمجة (API Reference)**](03_api_reference.md): توثيق كامل لكافة الدوال البرمجية ورموز الأخطاء.
*   [**الهيكلية الداخلية والأداء (Internals)**](04_internals.md): آليات التزامن الذري، وموانع الذاكرة، وخوارزميات التخصيص.
*   [**دليل تسريع الشبكات و AF_XDP**](NETWORK_TUNING.md): تهيئة الشبكات للاتصال الموزع بأزمنة استجابة منخفضة جداً.
