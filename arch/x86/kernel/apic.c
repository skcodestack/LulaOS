#include <arch/x86/apic.h>
#include <arch/x86/cpu.h>
#include <printk.h>
#include <arch/x86/highmem.h>
#include <arch/x86/msr.h>
#include <arch/x86/acpi.h>
#include <arch/x86/io.h>
#include <arch/x86/ptrace.h>
#include <interrupts/interrupts.h>
#include <libs/memcpy.h>


int isSupportApic(){
    int eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9));
}

int isSupportX2Apic(){
    int eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1 << 21));
}

int getApicId(){
    //return GET_APIC_ID(apic_read(APIC_ID));
    int eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ebx >> 24) & 0xFF;
}

void disable_8259_pic(){
    outb(0xff,0x21);
    outb(0xff,0xA1);
}

void local_apic_init(){
    remapping_apic();
    enable_hardware_apic();
    disable_8259_pic();
    
    unsigned int  apic_id =  GET_APIC_ID(apic_read(APIC_ID));
    printk("apic id id :%d\n",apic_id);

    apic_setup_lvts();

    unsigned int tpr =  apic_read(APIC_TPR);
    tpr &= ~APIC_TPR_MASK;
    tpr |=  APIC_TPR_VALUE(2,0); //accept above 32 vec
    apic_write(APIC_TPR,tpr);

    ///soft enbale apic
    unsigned int svr = apic_read(APIC_SPIV);
    svr &= ~APIC_VECTOR_MASK;
    svr |= APIC_SPIV_FOCUS_DISABLED;
    svr |= APIC_SPIV_APIC_ENABLED;
    svr |= SPURIOUS_APIC_VECTOR;
    apic_write(APIC_SPIV,svr);

    // APIC Timer 校准（使用 PIT Channel 2 作为参考）
    calibrate_apic_timer();

}

void apic_setup_lvts(){
    //mask 8259A INTER
    unsigned val =  apic_read(APIC_LINT0);
    val |= APIC_LVT_MASKED | APIC_DM_EXTINT; 
    apic_write(APIC_LINT0,val);

    val =  apic_read(APIC_LINT1);
    val |= APIC_DM_NMI | APIC_LVT_MASKED;
    apic_write(APIC_LINT1,val);

    apic_write(APIC_ERR,ERROR_APIC_VECTOR | APIC_DM_FIXED);
}

/*
 * calibrate_apic_timer() - 使用 PIT Channel 2 校准 APIC Timer
 *
 * 原理：
 *   1. PIT 8254 Channel 2 以 1193182Hz 固定频率运行
 *   2. 设置一个已知时间窗口（10ms），轮询 PIT Channel 2 输出位等待完成
 *   3. 在该窗口内让 APIC Timer 从最大值开始倒计时
 *   4. 窗口结束后读取已消耗的 tick 数，即为 10ms 内的 tick
 *   5. 将该值写入 INITCNT，设置为周期性模式
 */
void calibrate_apic_timer(){
    unsigned int end, elapsed;
    unsigned char gate_val;

    // ---- 步骤1：配置 PIT Channel 2（一次性触发，轮询模式）----
    // 读取端口 0x61，保持 speaker/turbo 位不变
    gate_val = inb(PIT_CH2_GATE);
    gate_val &= ~0x03;  // 清除 bit0(gate) 和 bit1(speaker)
    outb(gate_val, PIT_CH2_GATE);

    // 命令字：Channel 2, 先低后高, Mode 0(计数到0输出变高)
    outb(0xB0, PIT_CMD);

    // 写入校准计数值（~10ms）
    unsigned int pit_count = APIC_CAL_PIT_COUNT;
    outb(pit_count & 0xFF, PIT_CH2_DATA);         // 低字节
    outb((pit_count >> 8) & 0xFF, PIT_CH2_DATA);  // 高字节

    // 打开 Channel 2 gate，开始倒计时
    gate_val |= 0x01;  // 设置 bit0(gate=1)
    outb(gate_val, PIT_CH2_GATE);

    // ---- 步骤2：启动 APIC Timer（一次性模式）----
    // 16分频：DCR bit[2:1]=1, bit[0]=1 -> 0x3 -> 16分频
    apic_write(APIC_TIMER_DIVIDE, 0x3);
    // 屏蔽 Timer LVT（校准时不需要中断）
    apic_write(APIC_TIMER, APIC_LVT_MASKED);
    // 从最大值开始倒计时
    apic_write(APIC_TIMER_INITCNT, 0xFFFFFFFF);

    // ---- 步骤3：等待 PIT Channel 2 计数完成 ----
    // PIT 计数到 0 时，输出位 bit5(0x61) 变为 1
    while (!(inb(PIT_CH2_GATE) & 0x20))
        ;

    // ---- 步骤4：读取 APIC Timer 消耗量 ----
    end = apic_read(APIC_TIMER_CURRCNT);
    elapsed = 0xFFFFFFFF - end;

    // 屏蔽 Timer（停止倒计时）
    apic_write(APIC_TIMER, APIC_LVT_MASKED);

    printk("APIC Timer calibrate: %d ticks per %dms\n", elapsed, APIC_CAL_MS);

    if (elapsed == 0) {
        printk("APIC Timer calibrate failed: 0 ticks, timer may not work\n");
        return;
    }

    // ---- 步骤5：设置为周期性模式 ----
    apic_write(APIC_TIMER_INITCNT, elapsed);
    apic_write(APIC_TIMER, TIMER_APIC_VECTOR | APIC_TM_PERIODIC);

    printk("APIC Timer: INITCNT=%d, freq=%d Hz (approx)\n",
           elapsed, elapsed * (1000 / APIC_CAL_MS));
}


void enable_hardware_apic(){
    unsigned long l,h;
    rdmsr(MSR_IA32_APICBASE, l, h);
    printk("Local APIC . %d , %x\n",MSR_IA32_APICBASE_ENABLE & l,l);
    if (!(l & MSR_IA32_APICBASE_ENABLE)) {
        printk("Local APIC disabled by BIOS -- reenabling.\n");
        l &= ~MSR_IA32_APICBASE_BASE;
        l |= MSR_IA32_APICBASE_ENABLE | APIC_DEFAULT_PHYS_BASE;
        wrmsr(MSR_IA32_APICBASE, l, h);
    }
}

void enable_x2apic(){
    unsigned long l,h;
    rdmsr(MSR_IA32_APICBASE, l, h);
    if (!(l & MSR_IA32_APICBASE_X2_ENABLE)) {
        l |= MSR_IA32_APICBASE_X2_ENABLE | MSR_IA32_APICBASE_ENABLE;
        wrmsr(MSR_IA32_APICBASE, l, h);
    }
}

  


//remapping apic base addr
void remapping_apic(){ 
    unsigned long l,h;
    rdmsr(MSR_IA32_APICBASE, l, h);
    unsigned long apic_base_addr = l & 0xFFFFF000;
    printk("Local APIC ADDR %x\n",apic_base_addr);
    if(!apic_base_addr){
        apic_base_addr = APIC_DEFAULT_PHYS_BASE;
    }
    set_fixmap_nocache(FIX_APIC_BASE, apic_base_addr); 
}

void remapping_ioapic(){
    for (uint32_t i = 0; i < acpi_context.ioapic_count; i++) {
        unsigned long phy = acpi_context.ioapics[i].address;
        if (!phy) {
            phy = IOAPIC_DEFAULT_PHYS_BASE;
        }
        set_fixmap_nocache(FIX_IO_APIC_BASE_0 + i, phy);
    }
}


void io_apic_init(){
    remapping_ioapic();

    for (uint32_t i = 0; i < acpi_context.ioapic_count; i++) {
        unsigned long base = IOAPIC_BASE(i);

        unsigned int id = GET_IOAPIC_ID(ioapic_read(base, IOAPIC_ID));

        unsigned long version = ioapic_read(base, IOAPIC_VERSION);
        unsigned int version_code = GET_IOAPIC_VERSION(version);
        unsigned int rte_count = GET_IOAPIC_RTE_COUNT(version) + 1;

        printk("ioapic[%d] id:%d acpi_id:%d version:%d rte_count:%d\n",
            i, id, acpi_context.ioapics[i].id, version_code, rte_count);

        unsigned int gsi_base = acpi_context.ioapics[i].global_irq_base;

        /* 初始化本 IOAPIC 的所有 RTE pin */
        for (unsigned int pin = 0; pin < rte_count; pin++) {
            unsigned int gsi = gsi_base + pin;

            /* 默认：GSI N → vector = FIRST_DEVICE_VECTOR + N */
            unsigned int vector = FIRST_DEVICE_VECTOR + gsi;
            unsigned int trigger  = 0;  /* 边缘触发 */
            unsigned int polarity = 0;  /* 高有效 */

            /* 查找 ACPI 中断源覆盖，更新 GSI/trigger/polarity */
            for (uint32_t k = 0; k < acpi_context.int_src_ovr_count; k++) {
                struct acpi_table_int_src_ovr *ovr = &acpi_context.int_src_ovrs[k];
                if (ovr->bus == 0 && ovr->bus_irq == pin) {
                    /* 此 ISA IRQ 有覆盖映射 */
                    gsi = ovr->global_irq;
                    vector = FIRST_DEVICE_VECTOR + gsi;

                    /* polarity: 0=默认(高), 1=高有效, 3=低有效 */
                    if (ovr->flags.polarity == 3)
                        polarity = 1;
                    else if (ovr->flags.polarity == 1)
                        polarity = 0;

                    /* trigger: 0=默认(边缘), 1=边缘, 3=电平 */
                    if (ovr->flags.trigger == 3)
                        trigger = 1;
                    else if (ovr->flags.trigger == 1)
                        trigger = 0;

                    printk("  ISA IRQ%d -> GSI%d vector=%#x trigger=%d polarity=%d\n",
                        pin, gsi, vector, trigger, polarity);
                    break;
                }
            }

            /* 目标 CPU：BSP（Bootstrap Processor） */
            unsigned int dest = getApicId();

            /* 默认全部屏蔽，驱动可通过 ioapic_enable_irq(gsi) 解除 */
            ioapic_set_rte(gsi, vector, trigger, polarity, dest, 1);
        }
    }
}

/*
 * ioapic_set_rte - 配置一个 IOAPIC RTE 条目
 *
 * @gsi:      Global System Interrupt 编号
 * @vector:   中断向量号（0x20~0xFF）
 * @trigger:  触发模式  0=边缘, 1=电平
 * @polarity: 极性      0=高有效, 1=低有效
 * @dest:     目标 APIC ID（物理模式）
 * @mask:     屏蔽      1=屏蔽, 0=使能
 */
void ioapic_set_rte(unsigned int gsi, unsigned int vector,
                    unsigned int trigger, unsigned int polarity,
                    unsigned int dest, unsigned int mask)
{
    int idx = acpi_find_ioapic_by_gsi(gsi);
    if (idx < 0) return;

    unsigned long base = IOAPIC_BASE(idx);
    unsigned int pin   = gsi - acpi_context.ioapics[idx].global_irq_base;

    struct ioapic_rte_entry entry;
    memset(&entry, 0, sizeof(entry));

    entry.vector        = vector & 0xFF;
    entry.delivery_mode = APIC_DM_FIXED;
    entry.dest_mode     = 0;   /* 物理目标模式 */
    entry.pin_polarity  = polarity;
    entry.trigger_mode  = trigger;
    entry.mask          = mask;
    entry.dest.physical.physical_dest = dest & 0xF;

    __ioapic_write_entry(base, pin, entry);
}

/*
 * ioapic_enable_irq - 解除指定 GSI 的屏蔽，使能中断
 *
 *  
 */
void ioapic_enable_irq(unsigned int vector)
{
    unsigned int gsi = vector - FIRST_DEVICE_VECTOR;
    if(gsi < 0){
        return;
    }
    int idx = acpi_find_ioapic_by_gsi(gsi);
    if (idx < 0) return;

    unsigned long base = IOAPIC_BASE(idx);
    unsigned int pin   = gsi - acpi_context.ioapics[idx].global_irq_base;

    /* 读取当前低 32 位 */
    unsigned int lo = ioapic_read(base, 0x10 + 2 * pin);
    lo &= ~(1 << 16);  /* 清除 mask 位 */
    ioapic_write(base, 0x10 + 2 * pin, lo);
}

/*
 * ioapic_disable_irq - 屏蔽指定 GSI，禁用中断
 *
 *  
 */
void ioapic_disable_irq(unsigned int vector)
{
    unsigned int gsi = vector - FIRST_DEVICE_VECTOR;
    if(gsi < 0){
        return;
    }
    int idx = acpi_find_ioapic_by_gsi(gsi);
    if (idx < 0) return;

    unsigned long base = IOAPIC_BASE(idx);
    unsigned int pin   = gsi - acpi_context.ioapics[idx].global_irq_base;

    unsigned int lo = ioapic_read(base, 0x10 + 2 * pin);
    lo |= (1 << 16);   /* 置位 mask 位 */
    ioapic_write(base, 0x10 + 2 * pin, lo);
} 


// ★ 写入顺序：先高后低！
// 因为低32位包含 mask 位，如果先写低32位且 mask=0，
// 中断会立即生效，但此时高32位（目标CPU）可能还没配置好
void __ioapic_write_entry(unsigned long base, int pin, struct ioapic_rte_entry entry)
{
    // 先写高32位（目标CPU）
    ioapic_write(base, 0x11 + 2 * pin, *(((unsigned int *)&entry) + 1));
    // 后写低32位（向量、mask等）
    ioapic_write(base, 0x10 + 2 * pin, *(unsigned int *)&entry);
}

/*
 * send_ipi - 向目标 APIC ID 发送指定向量的 IPI
 *
 * 等待 ICR 空闲 → 写入目标 APIC ID → 写入 ICRLO 触发中断
 */
void send_ipi(int apic_id, int vector){
    /* 等待上次 IPI 交付完成（Delivery Status 位清零） */
    while (apic_read(APIC_ICRLO) & APIC_DS_PENDING)
        ;

    /* 写入目标 APIC ID（物理目标模式） */
    apic_write(APIC_ICRHI, ((unsigned long)apic_id) << 24);

    /* 写 ICRLO 触发 IPI：FIXED 交付 + ASSERT + 边缘触发 */
    apic_write(APIC_ICRLO,
        APIC_DM_FIXED |
        APIC_DEST_MODE_PHY |
        APIC_LEVEL_ASSERT |
        APIC_TRIGGER_MODE_EDGE |
        (unsigned int)vector);
}

/*
 * send_init_ipi - 发送 INIT IPI，复位目标 CPU
 */
static void send_init_ipi(int apic_id){
    while (apic_read(APIC_ICRLO) & APIC_DS_PENDING)
        ;

    apic_write(APIC_ICRHI, ((unsigned long)apic_id) << 24);

    /* INIT IPI：INIT 交付模式 + ASSERT */
    apic_write(APIC_ICRLO,
        APIC_DM_INIT |
        APIC_DEST_MODE_PHY |
        APIC_LEVEL_ASSERT |
        APIC_TRIGGER_MODE_EDGE);
}

/*
 * send_sipi - 发送 Startup IPI，vector = 物理入口页地址 / 0x1000
 */
static void send_sipi(int apic_id, unsigned int vector){
    while (apic_read(APIC_ICRLO) & APIC_DS_PENDING)
        ;

    apic_write(APIC_ICRHI, ((unsigned long)apic_id) << 24);

    /* SIPI：STARTUP 交付模式 + ASSERT + vector[7:0] */
    apic_write(APIC_ICRLO,
        APIC_DM_STARTUP |
        APIC_DEST_MODE_PHY |
        APIC_LEVEL_ASSERT |
        (vector & 0xFF));
}

/*
 * send_startup_ipi - 标准 SMP 启动流程（INIT-SIPI-SIPI）
 *
 * 1. INIT IPI（复位目标 AP，使其进入等待 SIPI 状态）
 * 2. 等待 10ms
 * 3. SIPI（告知 AP 入口物理页地址）
 * 4. 等待 200us
 * 5. 再次发送 SIPI（若 AP 未响应，重新唤醒）
 *
 * @apic_id:     目标 AP 的 LAPIC ID
 * @startup_page: AP 入口物理地址所在页号（= 物理地址 / 0x1000）
 */
void send_startup_ipi(int apic_id, unsigned int startup_page){
    /* Step 1: INIT IPI - 复位目标 AP */
    send_init_ipi(apic_id);
    printk("SMP: INIT IPI sent to AP %d\n", apic_id);

    /* Step 2: 等待 10ms（AP 完成复位） */
    udelay(10000);

    /* Step 3: SIPI - 告知 AP 实模式入口页 */
    send_sipi(apic_id, startup_page);
    printk("SMP: SIPI (vector=%#x) sent to AP %d\n", startup_page, apic_id);

    /* Step 4: 等待 200us */
    udelay(200);

    /* Step 5: 再次发送 SIPI（若 AP 未响应） */
    send_sipi(apic_id, startup_page);
    printk("SMP: second SIPI sent to AP %d\n", apic_id);
}

/*
 * udelay - 使用 PIT Channel 2 实现微秒级精确延迟
 *
 * 不依赖中断，轮询 PIT 输出位（端口 0x61 bit5）
 */
void udelay(unsigned int us){
    unsigned int ticks = (us * PIT_FREQ) / 1000000;
    if (ticks == 0) ticks = 1;
    if (ticks > 0xFFFF) ticks = 0xFFFF;

    unsigned char gate = inb(PIT_CH2_GATE);
    unsigned char saved = gate;

    /* 关闭 gate，防止上次计数残留 */
    gate &= ~0x01;
    outb(gate, PIT_CH2_GATE);

    /* Channel 2: Mode 0（终端计数），二进制计数 */
    outb(0xB0, PIT_CMD);
    outb(ticks & 0xFF, PIT_CH2_DATA);
    outb((ticks >> 8) & 0xFF, PIT_CH2_DATA);

    /* 开启 gate，开始计数 */
    gate |= 0x01;
    outb(gate, PIT_CH2_GATE);

    /* 等待 OUT 引脚变高（计数完成） */
    while (!(inb(PIT_CH2_GATE) & 0x20))
        ;

    /* 恢复 gate 状态 */
    outb(saved, PIT_CH2_GATE);
}


void _init_apic(){

    int supportApic =  isSupportApic();
    int supportX2Apic =  isSupportX2Apic();
    if(!supportApic){
        printk("Not support apic\n");
        return;
    }
    printk("support apic\n");  
    if(supportX2Apic){
        printk("support x2Apic\n"); 
    }else {
        printk("not support x2Apic\n");
    }
    
    local_apic_init();
    io_apic_init();
}


 