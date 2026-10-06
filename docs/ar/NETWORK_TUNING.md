# بروتوكول شبكة Bifrost - متطلبات النظام ودليل ضبط الأداء

## 📋 نظرة عامة

توفر مكتبة Bifrost نموذجين متكاملين للاتصال ونقل البيانات:

| الطبقة | التقنية المستخدمة | زمن الاستجابة | سيناريو الاستخدام |
|-------|------------------|---------------|-------------------|
| **المحلية (Local)** | الذاكرة المشتركة (Shared Memory) | أقل من ميكروثانية | اتصال فائق السرعة بين عمليات نفس النظام (IPC) |
| **الشبكية (Network)** | Raw Socket / AF_XDP | يعتمد على العتاد والشبكة | اتصال بين أنظمة وخوادم متعددة عبر الشبكة |
| **البوابة (Gateway)** | مسار متكامل (End-to-End) | شبكي + محلي | أنظمة موزعة تجمع بين الذاكرة المشتركة والشبكة |

---

## 🏗️ البنية المعمارية للنظام

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                             بنية نظام Bifrost                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│    ┌─────────────┐         ┌─────────────┐         ┌─────────────┐          │
│    │  Process A  │◀───────▶│   Bifrost   │◀───────▶│  Process B  │          │
│    │   الكاتب    │   SHM   │    القلب    │   SHM   │   القارئ    │          │
│    └─────────────┘         └──────┬──────┘         └─────────────┘          │
│                                   │                                         │
│                            ┌──────┴──────┐                                  │
│                            │   البوابة   │                                  │
│                            │  (Gateway)  │                                  │
│                            └──────┬──────┘                                  │
│                                   │ Raw Socket                              │
│                            ┌──────┴──────┐                                  │
│                            │   الشبكة    │                                  │
│                            │  (AF_XDP)   │                                  │
│                            └─────────────┘                                  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 🖥️ متطلبات النظام

### الحد الأدنى من المتطلبات

| المكون | المواصفات المطلوبة |
|--------|-------------------|
| **نظام التشغيل** | Linux 4.18+ (Ubuntu 20.04+, RHEL 8+) |
| **النواة** | 5.x أو أحدث موصى بها لأفضل دعم لـ AF_XDP |
| **المعالج** | x86_64 مع تعليمات SSE4.2، نواتين على الأقل |
| **الذاكرة** | 4 غيغابايت رام كحد أدنى |
| **الشبكة** | أي بطاقة إيثرنت (1Gbps فأعلى) |

### الموصى به لبيئات الإنتاج عالية الحمل

| المكون | المواصفات الموصى بها |
|--------|---------------------|
| **نظام التشغيل** | Ubuntu 22.04 LTS أو RHEL 8.6+ |
| **النواة** | Linux 5.15+ |
| **المعالج** | Intel Xeon أو AMD EPYC، 4+ أنوية مخصصة ومعزولة |
| **الذاكرة** | 16 غيغابايت فأعلى مع تفعيل الصفحات الضخمة (Huge Pages) |
| **الشبكة** | Intel i40e, ixgbe, Mellanox ConnectX (لدعم XDP native/zero-copy) |

### الحزم المطلوبة

```bash
# Ubuntu/Debian
sudo apt install -y build-essential meson ninja-build libbpf-dev libxdp-dev

# RHEL/CentOS
sudo dnf install -y gcc meson ninja-build libbpf-devel libxdp-devel
```

---

## ⚡ ضبط وتحسين أداء النظام (Performance Tuning)

### 1. عزل الأنوية (CPU Isolation)
تخصيص أنوية محددة لـ Bifrost لمنع مقاطعات مجدول النظام:

```bash
# في /etc/default/grub
GRUB_CMDLINE_LINUX="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3"
sudo update-grub && sudo reboot
```

### 2. تفعيل الصفحات الضخمة (Huge Pages)
تقليل إخفاقات الـ TLB في الذاكرة:

```bash
# حجز 1024 صفحة بحجم 2 ميغابايت
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages

# للتثبيت الدائم (أضف إلى /etc/sysctl.conf)
vm.nr_hugepages=1024
vm.hugetlb_shm_group=1000
```

### 3. ضبط مخازن مآخذ الشبكة (Network Buffers)

```bash
# زيادة أحجام مخازن القراءة والكتابة
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=67108864
sudo sysctl -w net.core.rmem_default=16777216
sudo sysctl -w net.core.wmem_default=16777216

# زيادة طابور التراكم
sudo sysctl -w net.core.netdev_max_backlog=300000

# تعطيل rp_filter للمآخذ الخام
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
```

### 4. تخصيص مقاطعات الشبكة (IRQ Affinity)

```bash
# معرفة المقاطعات الخاصة ببطاقة الشبكة
cat /proc/interrupts | grep eth0

# توجيه المقاطعة لنواة محددة
echo 2 | sudo tee /proc/irq/32/smp_affinity
```

### 5. تعطيل توفير الطاقة للمُعالج

```bash
# ضبط المجدول على وضع الأداء الأقصى
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee $cpu
done
```

---

## 🔧 مرجع المكونات والأكواد

| الملف | الوصف |
|------|-------|
| `include/bifrost_gateway.h` | ترويسة واجهة البوابة (Gateway API) |
| `src/bifrost_gateway.c` | تنفيذ البوابة وربط الذاكرة بالشبكة |
| `include/bifrost_xdp.h` | ترويسة تسريع وتجاوز النواة AF_XDP |
| `src/bifrost_xdp.c` | تنفيذ AF_XDP |
| `include/bifrost_netproto.h` | بروتوكول شبكة UDP الخاص بـ Bifrost |
| `src/bifrost_netproto.c` | تنفيذ بروتوكول UDP |
| `examples/bifrost_xdp_sender.c` | مرسل شبكي فائق الأداء |
| `examples/bifrost_xdp_receiver.c` | مستقبل شبكي فائق الأداء |
| `examples/bifrost_gateway_example.c` | مثال تطبيقي شامل للبوابة |

### استخدام البوابة في الكود البرمجي

```c
#include "bifrost_gateway.h"

// تهيئة إعدادات البوابة
bifrost_gateway_config_t config = bifrost_gateway_config_default();
config.interface = "enp0s31f6";
config.remote_mac = "aa:bb:cc:dd:ee:ff";
config.remote_ip = "192.168.1.100";
config.pool_name = "my_pool";
config.mode = BIFROST_GATEWAY_BRIDGE;

// إنشاء وتشغيل البوابة
bifrost_gateway_t* gw = bifrost_gateway_create(&config);
bifrost_gateway_run(gw);  // وضع حاجب، أو استخدام bifrost_gateway_process() داخل حلقة
bifrost_gateway_destroy(gw);
```

تصريف وتشغيل المثال:

```bash
# البناء باستخدام Meson / Ninja
meson compile -C build bifrost_gateway_example
# أو: ninja -C build bifrost_gateway_example

# التشغيل بصلاحيات الجذر
sudo ./build/examples/bifrost_gateway_example lo 00:00:00:00:00:00 127.0.0.1 shm2net
```

---

## 🐛 حل المشاكل الشائعة

### خطأ الصلاحيات (Permission Denied)
تتطلب مآخذ Raw Sockets و AF_XDP صلاحيات root، أو منح الصلاحية للملف التنفيذي:
```bash
sudo setcap cap_net_raw,cap_net_admin+ep ./build/examples/bifrost_xdp_sender
```

### بطاقة الشبكة غير موجودة (Interface Not Found)
تحقق من الأسماء الصحيحة لبطاقات الشبكة عبر الأمر:
```bash
ip link show
```
واستعمل الاسم الفعلي (مثل `enp0s31f6` وليس بالضرورة `eth0`).
