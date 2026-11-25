/*
 * virtio ccw machine
 *
 * Copyright 2012, 2020 IBM Corp.
 * Copyright (c) 2009 Alexander Graf <agraf@suse.de>
 * Author(s): Cornelia Huck <cornelia.huck@de.ibm.com>
 *            Janosch Frank <frankja@linux.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/confidential-guest-support.h"
#include "hw/core/boards.h"
#include "hw/s390x/sclp.h"
#include "hw/s390x/s390_flic.h"
#include "virtio-ccw.h"
#include "qemu/config-file.h"
#include "qemu/ctype.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/qemu-print.h"
#include "qemu/units.h"
#include "hw/s390x/s390-pci-bus.h"
#include "system/reset.h"
#include "hw/s390x/storage-keys.h"
#include "hw/s390x/storage-attributes.h"
#include "hw/s390x/event-facility.h"
#include "ipl.h"
#include "hw/s390x/s390-virtio-ccw.h"
#include "hw/s390x/ebcdic.h"
#include "hw/s390x/css-bridge.h"
#include "hw/s390x/ap-bridge.h"
#include "migration/register.h"
#include "target/s390x/cpu_models.h"
#include "hw/core/nmi.h"
#include "hw/core/qdev-properties.h"
#include "hw/s390x/tod.h"
#include "system/system.h"
#include "system/cpus.h"
#include "system/hostmem.h"
#include "target/s390x/kvm/pv.h"
#include "migration/blocker.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/qapi-type-infos-machine-s390x.h"
#include "qapi/visitor.h"
#include "qapi/qapi-visit-machine-s390x.h"
#include "hw/s390x/cpu-topology.h"
#include "kvm/kvm_s390x.h"
#include "hw/virtio/virtio-md-pci.h"
#include "hw/s390x/virtio-ccw-md.h"
#include "system/replay.h"
#include CONFIG_DEVICES

static Error *pv_mig_blocker;

static S390CPU *s390x_new_cpu(const char *typename, uint32_t core_id,
                              Error **errp)
{
    S390CPU *cpu = S390_CPU(object_new(typename));
    S390CPU *ret = NULL;

    if (!object_property_set_int(OBJECT(cpu), "core-id", core_id, errp)) {
        goto out;
    }
    if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
        goto out;
    }
    ret = cpu;

out:
    object_unref(OBJECT(cpu));
    return ret;
}

static void s390_init_cpus(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_CLASS(mc);
    int i;

    if (machine->smp.threads > s390mc->max_threads) {
        error_report("S390 does not support more than %d threads.",
                     s390mc->max_threads);
        exit(1);
    }

    /* initialize possible_cpus */
    mc->possible_cpu_arch_ids(machine);

    for (i = 0; i < machine->smp.cpus; i++) {
        s390x_new_cpu(machine->cpu_type, i, &error_fatal);
    }
}

static const char *const reset_dev_types[] = {
    TYPE_VIRTUAL_CSS_BRIDGE,
    "s390-sclp-event-facility",
    "s390-flic",
    "diag288",
    TYPE_S390_PCI_HOST_BRIDGE,
    TYPE_AP_BRIDGE,
};

static void subsystem_reset(void)
{
    DeviceState *dev;
    int i;

    /*
     * ISM firmware is sensitive to unexpected changes to the IOMMU, which can
     * occur during reset of the vfio-pci device (unmap of entire aperture).
     * Ensure any passthrough ISM devices are reset now, while CPUs are paused
     * but before vfio-pci cleanup occurs.
     */
    s390_pci_ism_reset();

    for (i = 0; i < ARRAY_SIZE(reset_dev_types); i++) {
        dev = DEVICE(object_resolve_path_type("", reset_dev_types[i], NULL));
        if (dev) {
            device_cold_reset(dev);
        }
    }
    if (s390_has_topology()) {
        s390_topology_reset();
    }
}

static void s390_set_memory_limit(S390CcwMachineState *s390ms,
                                  uint64_t new_limit)
{
    uint64_t hw_limit = 0;
    int ret = 0;

    assert(!s390ms->memory_limit && new_limit);
    if (kvm_enabled()) {
        ret = kvm_s390_set_mem_limit(new_limit, &hw_limit);
    }
    if (ret == -E2BIG) {
        error_report("host supports a maximum of %" PRIu64 " GB",
                     hw_limit / GiB);
        exit(EXIT_FAILURE);
    } else if (ret) {
        error_report("setting the guest size failed");
        exit(EXIT_FAILURE);
    }
    s390ms->memory_limit = new_limit;
}

static void s390_set_max_pagesize(S390CcwMachineState *s390ms,
                                  uint64_t pagesize)
{
    assert(!s390ms->max_pagesize && pagesize);
    if (kvm_enabled()) {
        kvm_s390_set_max_pagesize(pagesize, &error_fatal);
    }
    s390ms->max_pagesize = pagesize;
}

static void s390_memory_init(MachineState *machine)
{
    S390CcwMachineState *s390ms = S390_CCW_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = machine->ram;
    uint64_t ram_size = memory_region_size(ram);
    uint64_t devmem_base, devmem_size;

    if (!QEMU_IS_ALIGNED(ram_size, 1 * MiB)) {
        /*
         * SCLP cannot possibly expose smaller granularity right now and KVM
         * cannot handle smaller granularity. As we don't support NUMA, the
         * region size directly corresponds to machine->ram_size, and the region
         * is a single RAM memory region.
         */
        error_report("ram size must be multiples of 1 MiB");
        exit(EXIT_FAILURE);
    }

    devmem_size = 0;
    devmem_base = ram_size;
#ifdef CONFIG_MEM_DEVICE
    if (machine->ram_size < machine->maxram_size) {

        /*
         * Make sure memory devices have a sane default alignment, even
         * when weird initial memory sizes are specified.
         */
        devmem_base = QEMU_ALIGN_UP(devmem_base, 1 * GiB);
        devmem_size = machine->maxram_size - machine->ram_size;
    }
#endif
    s390_set_memory_limit(s390ms, devmem_base + devmem_size);

    /* Map the initial memory. Must happen after setting the memory limit. */
    memory_region_add_subregion(sysmem, 0, ram);

    /* Initialize address space for memory devices. */
#ifdef CONFIG_MEM_DEVICE
    if (devmem_size) {
        machine_memory_devices_init(machine, devmem_base, devmem_size);
    }
#endif /* CONFIG_MEM_DEVICE */

    /*
     * Configure the maximum page size. As no memory devices were created
     * yet, this is the page size of initial memory only.
     */
    s390_set_max_pagesize(s390ms, qemu_maxrampagesize());
    /* Initialize storage key device */
    s390_skeys_init();
    /* Initialize storage attributes device */
    s390_stattrib_init();
}

static void s390_init_ipl_dev(const char *kernel_filename,
                              const char *kernel_cmdline,
                              const char *initrd_filename, const char *firmware,
                              bool enforce_bios)
{
    Object *new = object_new(TYPE_S390_IPL);
    DeviceState *dev = DEVICE(new);

    if (kernel_filename) {
        qdev_prop_set_string(dev, "kernel", kernel_filename);
    }
    if (initrd_filename) {
        qdev_prop_set_string(dev, "initrd", initrd_filename);
    }
    qdev_prop_set_string(dev, "cmdline", kernel_cmdline);
    qdev_prop_set_string(dev, "firmware", firmware);
    qdev_prop_set_bit(dev, "enforce_bios", enforce_bios);
    object_property_add_child(qdev_get_machine(), TYPE_S390_IPL,
                              new);
    object_unref(new);
    qdev_realize(dev, NULL, &error_fatal);
}

static void s390_create_virtio_net(BusState *bus, const char *name)
{
    DeviceState *dev;
    int cnt = 0;

    while ((dev = qemu_create_nic_device(name, true, "virtio"))) {
        g_autofree char *childname = g_strdup_printf("%s[%d]", name, cnt++);
        object_property_add_child(OBJECT(bus), childname, OBJECT(dev));
        qdev_realize_and_unref(dev, bus, &error_fatal);
    }
}

static void s390_create_sclpconsole(SCLPDevice *sclp,
                                    const char *type, Chardev *chardev)
{
    SCLPEventFacility *ef = sclp->event_facility;
    BusState *ev_fac_bus = sclp_get_event_facility_bus(ef);
    DeviceState *dev;

    dev = qdev_new(type);
    object_property_add_child(OBJECT(ef), type, OBJECT(dev));
    qdev_prop_set_chr(dev, "chardev", chardev);
    qdev_realize_and_unref(dev, ev_fac_bus, &error_fatal);
}

static void s390_create_sclpcpi(SCLPDevice *sclp)
{
    SCLPEventFacility *ef = sclp->event_facility;
    BusState *ev_fac_bus = sclp_get_event_facility_bus(ef);
    DeviceState *dev;

    dev = qdev_new(TYPE_SCLP_EVENT_CPI);
    object_property_add_child(OBJECT(ef), "sclpcpi", OBJECT(dev));
    qdev_realize_and_unref(dev, ev_fac_bus, &error_fatal);
}

static void ccw_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_CLASS(mc);
    S390CcwMachineState *ms = S390_CCW_MACHINE(machine);
    int ret;
    VirtualCssBus *css_bus;
    DeviceState *dev;

    if (ms->esa390_cm && !kvm_enabled()) {
        error_report("machine property esa390-cm=on requires KVM");
        exit(1);
    }

    ms->sclp = SCLP(object_new(TYPE_SCLP));
    object_property_add_child(OBJECT(machine), TYPE_SCLP, OBJECT(ms->sclp));
    qdev_realize_and_unref(DEVICE(ms->sclp), NULL, &error_fatal);

    /* init memory + setup max page size. Required for the CPU model */
    s390_memory_init(machine);

    /* init CPUs (incl. CPU model) early so s390_has_feature() works */
    s390_init_cpus(machine);

    /* Need CPU model to be determined before we can set up PV */
    if (machine->cgs) {
        confidential_guest_kvm_init(machine->cgs, &error_fatal);
    }

    s390_flic_init();

    /* init the SIGP facility */
    s390_init_sigp();

    /* create AP bridge and bus(es) */
    s390_init_ap();

    /* get a BUS */
    css_bus = virtual_css_bus_init();
    s390_init_ipl_dev(machine->kernel_filename, machine->kernel_cmdline,
                      machine->initrd_filename,
                      machine->firmware ?: "s390-ccw.img",
                      true);

    dev = qdev_new(TYPE_S390_PCI_HOST_BRIDGE);
    object_property_add_child(qdev_get_machine(), TYPE_S390_PCI_HOST_BRIDGE,
                              OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    s390_enable_css_support(s390_cpu_addr2state(0));

    ret = css_create_css_image(VIRTUAL_CSSID, true);
    assert(ret == 0);

    css_register_vmstate();

    /* Create VirtIO network adapters */
    s390_create_virtio_net(BUS(css_bus), mc->default_nic);

    /* init consoles */
    {
        bool lm_first = s390mc->line_mode_console_first;

        if (serial_hd(0)) {
            s390_create_sclpconsole(ms->sclp,
                                    lm_first ? "sclplmconsole" : "sclpconsole",
                                    serial_hd(0));
        }
        if (serial_hd(1)) {
            s390_create_sclpconsole(ms->sclp,
                                    lm_first ? "sclpconsole" : "sclplmconsole",
                                    serial_hd(1));
        }
    }

    /* init the TOD clock */
    s390_init_tod();

    /* init SCLP event Control-Program Identification */
    if (s390mc->use_cpi) {
        s390_create_sclpcpi(ms->sclp);
    }

}

static void s390_cpu_plug(const HotplugHandler *hotplug_dev,
                          DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    MachineState *ms = MACHINE(hotplug_dev);
    S390CPU *cpu = S390_CPU(dev);

    g_assert(!ms->possible_cpus->cpus[cpu->env.core_id].cpu);
    ms->possible_cpus->cpus[cpu->env.core_id].cpu = CPU(dev);

    if (s390_has_topology()) {
        s390_topology_setup_cpu(ms, cpu, errp);
        if (*errp) {
            return;
        }
    }

    if (dev->hotplugged) {
        raise_irq_cpu_hotplug();
    }
}

static void s390_do_cpu_reset(CPUState *cs, run_on_cpu_data arg)
{
    resettable_reset(OBJECT(cs), RESET_TYPE_S390_CPU_NORMAL);
}

static void s390_do_cpu_initial_reset(CPUState *cs, run_on_cpu_data arg)
{
    resettable_reset(OBJECT(cs), RESET_TYPE_S390_CPU_INITIAL);
}

static void s390_do_cpu_load_normal(CPUState *cs, run_on_cpu_data arg)
{
    S390CPUClass *scc = S390_CPU_GET_CLASS(cs);

    scc->load_normal(cs);
}

static void s390_do_cpu_ipl(CPUState *cs, run_on_cpu_data arg)
{
    S390CPU *cpu = S390_CPU(cs);

    s390_ipl_prepare_cpu(cpu);
    s390_cpu_set_state(S390_CPU_STATE_OPERATING, cpu);
}

static void s390_machine_unprotect(S390CcwMachineState *ms)
{
    if (!s390_pv_vm_try_disable_async(ms)) {
        s390_pv_vm_disable();
    }
    ms->pv = false;
    migrate_del_blocker(&pv_mig_blocker);
    ram_block_discard_disable(false);
}

static int s390_machine_protect(S390CcwMachineState *ms,
                                struct S390PVResponse *pv_resp)
{
    Error *local_err = NULL;
    int rc;

   /*
    * Discarding of memory in RAM blocks does not work as expected with
    * protected VMs. Sharing and unsharing pages would be required. Disable
    * it for now, until until we have a solution to make at least Linux
    * guests either support it (e.g., virtio-balloon) or fail gracefully.
    */
    rc = ram_block_discard_disable(true);
    if (rc) {
        error_report("protected VMs: cannot disable RAM discard");
        return rc;
    }

    error_setg(&pv_mig_blocker,
               "protected VMs are currently not migratable.");
    rc = migrate_add_blocker(&pv_mig_blocker, &local_err);
    if (rc) {
        ram_block_discard_disable(false);
        error_report_err(local_err);
        return rc;
    }

    /* Create SE VM */
    rc = s390_pv_vm_enable();
    if (rc) {
        ram_block_discard_disable(false);
        migrate_del_blocker(&pv_mig_blocker);
        return rc;
    }

    ms->pv = true;

    /* Will return 0 if API is not available since it's not vital */
    rc = s390_pv_query_info();
    if (rc) {
        goto out_err;
    }

    /* Set SE header and unpack */
    rc = s390_ipl_prepare_pv_header(pv_resp, &local_err);
    if (rc) {
        goto out_err;
    }

    /* Decrypt image */
    rc = s390_ipl_pv_unpack(pv_resp);
    if (rc) {
        goto out_err;
    }

    /* Verify integrity */
    rc = s390_pv_verify(pv_resp);
    if (rc) {
        goto out_err;
    }
    return rc;

out_err:
    if (local_err) {
        error_report_err(local_err);
    }
    s390_machine_unprotect(ms);
    return rc;
}

static void s390_pv_prepare_reset(S390CcwMachineState *ms)
{
    CPUState *cs;

    if (!s390_is_pv()) {
        return;
    }
    /* Unsharing requires all cpus to be stopped */
    CPU_FOREACH(cs) {
        s390_cpu_set_state(S390_CPU_STATE_STOPPED, S390_CPU(cs));
    }
    s390_pv_unshare();
    s390_pv_prep_reset();
}

static SubchDev *s390_get_ipl_subchannel(S390CcwMachineState *ms)
{
    IplParameterBlock *iplb = s390_ipl_get_iplb();
    DeviceState *dev = ms->ipl_device ?: get_boot_device(0);
    CcwDevice *ccw_dev;

    if (!dev || !iplb || iplb->pbt != S390_IPL_TYPE_CCW) {
        return NULL;
    }
    ccw_dev = (CcwDevice *)object_dynamic_cast(OBJECT(dev), TYPE_CCW_DEVICE);
    if (!ccw_dev || ccw_dev->sch->ssid != (iplb->ccw.ssid & 3) ||
        ccw_dev->sch->devno != be16_to_cpu(iplb->ccw.devno)) {
        return NULL;
    }
    return ccw_dev->sch;
}

static void s390_machine_reset(MachineState *machine, ResetType type)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(machine);
    struct S390PVResponse pv_resp;
    enum s390_reset reset_type;
    CPUState *cs, *t;
    S390CPU *cpu;
    SubchDev *ipl_sch = NULL;
    uint8_t ipl_lpum = 0;

    /*
     * Temporarily drop the record/replay mutex to let rr_cpu_thread_fn()
     * process the run_on_cpu() requests below. This is safe, because at this
     * point one of the following is true:
     * - All CPU threads are not running, either because the machine is being
     *   initialized, or because the guest requested a reset using diag 308.
     *   There is no risk to desync the record/replay state.
     * - A snapshot is about to be loaded. The record/replay state consistency
     *   is not important.
     */
    replay_mutex_unlock();

    /* get the reset parameters, reset them once done */
    s390_ipl_get_reset_request(&cs, &reset_type);

    /* all CPUs are paused and synchronized at this point */
    s390_cmma_reset();

    /*
     * A configuration with the ESA/390-compatibility mode facility is
     * put (back) into that mode by every reset that belongs to a load or
     * clears the configuration; only a CPU reset or a system-reset-normal
     * would leave the mode alone, and those do not come through here.
     */
    if (ms->esa390_cm && reset_type != S390_RESET_PV) {
        if (kvm_s390_set_arch_mode(true)) {
            error_report("could not put the configuration into the "
                         "ESA/390-compatibility mode");
        }
        CPU_FOREACH(t) {
            S390_CPU(t)->env.esa_mode = true;
        }
    }

    cpu = S390_CPU(cs);

    switch (reset_type) {
    case S390_RESET_EXTERNAL:
    case S390_RESET_REIPL:
        /*
         * Reset the subsystem which includes a AP reset. If a PV
         * guest had APQNs attached the AP reset is a prerequisite to
         * unprotecting since the UV checks if all APQNs are reset.
         */
        subsystem_reset();
        if (s390_is_pv()) {
            s390_machine_unprotect(ms);
        }

        /*
         * Device reset includes CPU clear resets so this has to be
         * done AFTER the unprotect call above.
         */
        qemu_devices_reset(type);
        s390_crypto_reset();

        /*
         * Loading a snapshot replaces the machine state right after this
         * reset, so there is nothing to IPL.  A channel-program IPL would
         * even hang: its I/O waits for block devices that are drained for
         * the load.
         */
        if (type == RESET_TYPE_SNAPSHOT_LOAD && s390_ipl_native()) {
            break;
        }

        /* configure and start the ipl CPU only */
        run_on_cpu(cs, s390_do_cpu_ipl, RUN_ON_CPU_NULL);
        break;
    case S390_RESET_MODIFIED_CLEAR:
        /*
         * Subsystem reset needs to be done before we unshare memory
         * and lose access to VIRTIO structures in guest memory.
         */
        subsystem_reset();
        s390_crypto_reset();
        s390_pv_prepare_reset(ms);
        CPU_FOREACH(t) {
            run_on_cpu(t, s390_do_cpu_full_reset, RUN_ON_CPU_NULL);
        }
        if (s390_ipl_native()) {
            run_on_cpu(cs, s390_do_cpu_ipl, RUN_ON_CPU_NULL);
            break;
        }
        run_on_cpu(cs, s390_do_cpu_load_normal, RUN_ON_CPU_NULL);
        break;
    case S390_RESET_LOAD_NORMAL:
        /*
         * Subsystem reset needs to be done before we unshare memory
         * and lose access to VIRTIO structures in guest memory.
         *
         * The firmware has already performed the IPL channel program at
         * this point.  A hardware IPL performs the reset first and leaves
         * the selected path recorded in LPUM after the successful I/O.
         * Preserve that result across the firmware's DIAG 308 reset so the
         * loaded operating system observes the architectural IPL state.
         */
        ipl_sch = s390_get_ipl_subchannel(ms);
        if (ipl_sch) {
            ipl_lpum = ipl_sch->curr_status.pmcw.lpum;
        }
        subsystem_reset();
        if (ipl_lpum) {
            ipl_sch->curr_status.pmcw.lpum = ipl_lpum;
        }
        s390_pv_prepare_reset(ms);
        CPU_FOREACH(t) {
            if (t == cs) {
                continue;
            }
            run_on_cpu(t, s390_do_cpu_reset, RUN_ON_CPU_NULL);
        }
        run_on_cpu(cs, s390_do_cpu_initial_reset, RUN_ON_CPU_NULL);
        if (s390_ipl_native()) {
            run_on_cpu(cs, s390_do_cpu_ipl, RUN_ON_CPU_NULL);
            break;
        }
        run_on_cpu(cs, s390_do_cpu_load_normal, RUN_ON_CPU_NULL);
        break;
    case S390_RESET_PV: /* Subcode 10 */
        subsystem_reset();
        s390_crypto_reset();

        CPU_FOREACH(t) {
            if (t == cs) {
                continue;
            }
            run_on_cpu(t, s390_do_cpu_full_reset, RUN_ON_CPU_NULL);
        }
        run_on_cpu(cs, s390_do_cpu_reset, RUN_ON_CPU_NULL);

        if (s390_machine_protect(ms, &pv_resp)) {
            s390_pv_inject_reset_error(cs, pv_resp);
            /*
             * Continue after the diag308 so the guest knows something
             * went wrong.
             */
            s390_cpu_set_state(S390_CPU_STATE_OPERATING, cpu);
            goto out_lock;
        }

        run_on_cpu(cs, s390_do_cpu_load_normal, RUN_ON_CPU_NULL);
        break;
    default:
        g_assert_not_reached();
    }

    CPU_FOREACH(t) {
        run_on_cpu(t, s390_do_cpu_set_diag318, RUN_ON_CPU_HOST_ULONG(0));
    }
    s390_ipl_clear_reset_request();

out_lock:
    /*
     * Re-take the record/replay mutex, temporarily dropping the BQL in order
     * to satisfy the ordering requirements.
     */
    bql_unlock();
    replay_mutex_lock();
    bql_lock();
}

static void s390_machine_device_pre_plug(const HotplugHandler *hotplug_dev,
                                         DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_CCW)) {
        virtio_ccw_md_pre_plug(VIRTIO_MD_CCW(dev), MACHINE(hotplug_dev), errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_PCI)) {
        virtio_md_pci_pre_plug(VIRTIO_MD_PCI(dev), MACHINE(hotplug_dev), errp);
    }
}

static void s390_machine_device_plug(const HotplugHandler *hotplug_dev,
                                     DeviceState *dev, Error **errp)
{
    S390CcwMachineState *s390ms = S390_CCW_MACHINE(hotplug_dev);

    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        s390_cpu_plug(hotplug_dev, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_CCW) ||
               object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_PCI)) {
        /*
         * At this point, the device is realized and set all memdevs mapped, so
         * qemu_maxrampagesize() will pick up the page sizes of these memdevs
         * as well. Before we plug the device and expose any RAM memory regions
         * to the system, make sure we don't exceed the previously set max page
         * size. While only relevant for KVM, there is not really any use case
         * for this with TCG, so we'll unconditionally reject it.
         */
        if (qemu_maxrampagesize() != s390ms->max_pagesize) {
            error_setg(errp, "Memory device uses a bigger page size than"
                       " initial memory");
            return;
        }
        if (object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_CCW)) {
            virtio_ccw_md_plug(VIRTIO_MD_CCW(dev), MACHINE(hotplug_dev), errp);
        } else {
            virtio_md_pci_plug(VIRTIO_MD_PCI(dev), MACHINE(hotplug_dev), errp);
        }
    }
}

static void s390_machine_device_unplug_request(const HotplugHandler *hotplug_dev,
                                               DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        error_setg(errp, "CPU hot unplug not supported on this machine");
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_CCW)) {
        virtio_ccw_md_unplug_request(VIRTIO_MD_CCW(dev), MACHINE(hotplug_dev),
                                     errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_PCI)) {
        virtio_md_pci_unplug_request(VIRTIO_MD_PCI(dev), MACHINE(hotplug_dev),
                                     errp);
    }
}

static void s390_machine_device_unplug(const HotplugHandler *hotplug_dev,
                                       DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_CCW)) {
        virtio_ccw_md_unplug(VIRTIO_MD_CCW(dev), MACHINE(hotplug_dev), errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_PCI)) {
        virtio_md_pci_unplug(VIRTIO_MD_PCI(dev), MACHINE(hotplug_dev), errp);
    }
 }

static CpuInstanceProperties s390_cpu_index_to_props(MachineState *ms,
                                                     unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static const CPUArchIdList *s390_possible_cpu_arch_ids(MachineState *ms)
{
    int i;
    unsigned int max_cpus = ms->smp.max_cpus;

    if (ms->possible_cpus) {
        g_assert(ms->possible_cpus && ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (i = 0; i < ms->possible_cpus->len; i++) {
        CpuInstanceProperties *props = &ms->possible_cpus->cpus[i].props;

        ms->possible_cpus->cpus[i].type = ms->cpu_type;
        ms->possible_cpus->cpus[i].vcpus_count = 1;
        ms->possible_cpus->cpus[i].arch_id = i;

        props->has_core_id = true;
        props->core_id = i;
        props->has_socket_id = true;
        props->socket_id = s390_std_socket(i, &ms->smp);
        props->has_book_id = true;
        props->book_id = s390_std_book(i, &ms->smp);
        props->has_drawer_id = true;
        props->drawer_id = s390_std_drawer(i, &ms->smp);
    }

    return ms->possible_cpus;
}

static const HotplugHandler *s390_get_hotplug_handler(MachineState *machine,
                                                      DeviceState *dev)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU) ||
        object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_CCW) ||
        object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MD_PCI)) {
        return HOTPLUG_HANDLER(machine);
    }
    return NULL;
}

static void s390_nmi(NMIState *ns)
{
    s390_cpu_restart(S390_CPU(first_cpu));
}

static inline bool machine_get_aes_key_wrap(Object *obj, Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    return ms->aes_key_wrap;
}

static inline void machine_set_aes_key_wrap(Object *obj, bool value,
                                            Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    ms->aes_key_wrap = value;
}

static inline bool machine_get_esa390_cm(Object *obj, Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    return ms->esa390_cm;
}

static inline void machine_set_esa390_cm(Object *obj, bool value,
                                         Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    ms->esa390_cm = value;
}

static inline bool machine_get_dea_key_wrap(Object *obj, Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    return ms->dea_key_wrap;
}

static inline void machine_set_dea_key_wrap(Object *obj, bool value,
                                            Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    ms->dea_key_wrap = value;
}

static void machine_get_loadparm(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    char *str = g_strndup((char *) ms->loadparm, sizeof(ms->loadparm));

    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static void machine_set_loadparm(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    char *val;

    if (!visit_type_str(v, name, &val, errp)) {
        return;
    }

    s390_ipl_fmt_loadparm(ms->loadparm, val, errp);
    g_free(val);
}

static void machine_get_chsc_config_token(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    static const char hex[] = "0123456789abcdef";
    g_autofree char *str = NULL;
    unsigned int i;

    if (!ms->chsc_config_token_set) {
        str = g_strdup("");
    } else {
        str = g_malloc(2 * sizeof(ms->chsc_config_token) + 1);
        for (i = 0; i < sizeof(ms->chsc_config_token); i++) {
            str[2 * i] = hex[ms->chsc_config_token[i] >> 4];
            str[2 * i + 1] = hex[ms->chsc_config_token[i] & 0xf];
        }
        str[2 * sizeof(ms->chsc_config_token)] = '\0';
    }
    visit_type_str(v, name, &str, errp);
}

static void machine_set_chsc_config_token(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    uint8_t token[sizeof(ms->chsc_config_token)];
    g_autofree char *str = NULL;
    unsigned int i;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }
    if (phase_check(PHASE_MACHINE_READY)) {
        error_setg(errp, "CHSC configuration token cannot be changed after "
                   "machine startup");
        return;
    }
    if (strlen(str) != 2 * sizeof(token)) {
        error_setg(errp, "CHSC configuration token must contain exactly "
                   "%zu hexadecimal digits", 2 * sizeof(token));
        return;
    }
    for (i = 0; i < sizeof(token); i++) {
        int high = g_ascii_xdigit_value(str[2 * i]);
        int low = g_ascii_xdigit_value(str[2 * i + 1]);

        if (high < 0 || low < 0) {
            error_setg(errp, "CHSC configuration token contains a non-hex "
                       "character at offset %u", 2 * i + (high >= 0));
            return;
        }
        token[i] = high << 4 | low;
    }
    memcpy(ms->chsc_config_token, token, sizeof(token));
    ms->chsc_config_token_set = true;
}

static void machine_get_ipl(Object *obj, Visitor *v,
                            const char *name, void *opaque, Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    g_autofree char *str = NULL;

    if (!ms->ipl_devno_set) {
        str = g_strdup("");
    } else if (ms->ipl_devno_full) {
        str = g_strdup_printf("%x.%x.%04x", ms->ipl_devno.cssid,
                              ms->ipl_devno.ssid, ms->ipl_devno.devid);
    } else {
        str = g_strdup_printf("%04x", ms->ipl_devno.devid);
    }
    visit_type_str(v, name, &str, errp);
}

static void machine_set_ipl(Object *obj, Visitor *v,
                            const char *name, void *opaque, Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    g_autofree char *str = NULL;
    unsigned int cssid, ssid, devno;
    int num, n1, n2;
    size_t len;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }
    if (phase_check(PHASE_MACHINE_READY)) {
        error_setg(errp, "IPL device cannot be changed after machine startup");
        return;
    }

    len = strlen(str);
    if (!strchr(str, '.')) {
        if (len < 1 || len > 4 ||
            strspn(str, "0123456789abcdefABCDEF") != len ||
            sscanf(str, "%x", &devno) != 1) {
            error_setg(errp, "Invalid IPL device address '%s'; expected "
                       "1-4 hexadecimal digits or cssid.ssid.devno", str);
            return;
        }
        ms->ipl_devno = (CssDevId) { .devid = devno, .valid = true };
        ms->ipl_devno_full = false;
    } else {
        num = sscanf(str, "%2x.%1x%n.%4x%n", &cssid, &ssid, &n1,
                     &devno, &n2);
        if (num != 3 || (n2 - n1) != 5 || len != n2 ||
            cssid > MAX_CSSID || ssid > MAX_SSID) {
            error_setg(errp, "Invalid IPL device address '%s'; expected "
                       "cssid.ssid.devno (for example fe.0.1000)", str);
            return;
        }
        ms->ipl_devno = (CssDevId) {
            .cssid = cssid, .ssid = ssid, .devid = devno, .valid = true,
        };
        ms->ipl_devno_full = true;
    }
    ms->ipl_devno_set = true;
}

static void s390_machine_ipl_notify(Notifier *notifier, void *data)
{
    S390CcwMachineState *ms = container_of(notifier, S390CcwMachineState,
                                           ipl_notifier);

    s390_ipl_validate_ipl_device(ms, &error_fatal);
}

static void machine_get_boot_certs(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    BootCertificatesList **certs = &ms->boot_certs;

    visit_type_BootCertificatesList(v, name, certs, errp);
}

static void machine_set_boot_certs(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_GET_CLASS(obj);
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    BootCertificatesList *cert_list = NULL;

    if (!s390mc->use_certs) {
        error_setg(errp, "boot-certs is not supported by this machine version");
        return;
    }

    visit_type_BootCertificatesList(v, name, &cert_list, errp);
    if (!cert_list) {
        return;
    }

    ms->boot_certs = cert_list;
}

static bool machine_get_ext_measurement_block(Object *obj, Error **errp)
{
    return S390_CCW_MACHINE(obj)->ext_measurement_block;
}

static void machine_set_ext_measurement_block(Object *obj, bool value,
                                              Error **errp)
{
    S390_CCW_MACHINE(obj)->ext_measurement_block = value;
}

bool s390_css_ext_measurement_block(void)
{
    Object *obj = qdev_get_machine();

    if (!obj || !object_dynamic_cast(obj, TYPE_S390_CCW_MACHINE)) {
        return true;
    }
    return S390_CCW_MACHINE(obj)->ext_measurement_block;
}

static inline bool machine_get_secure_boot(Object *obj, Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    return ms->secure_boot;
}

static inline void machine_set_secure_boot(Object *obj, bool value,
                                            Error **errp)
{
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_GET_CLASS(obj);
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    if (!s390mc->use_secure) {
        error_setg(errp, "secure-boot is not supported by this machine version");
        return;
    }

    ms->secure_boot = value;
}

static inline bool machine_get_pv(Object *obj, Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    return ms->pv;
}

 /*
  * S390x-specific global compatibility properties.
  *
  * On the s390 kernel, legacy virtio-pci is not usable because I/O BARs
  * are not supported (IO_SPACE_LIMIT is 0), and would only result in
  * unusable BARs and firmware warnings.
  *
  * Therefore, starting from v11.1, disable legacy virtio-pci by default,
  * while older machine types keep legacy behavior for compatibility.
  */
static GlobalProperty hw_compat_s390x[] = {
    { TYPE_VIRTIO_PCI, "disable-legacy", "on", .optional = true},
};
static const size_t hw_compat_s390x_len = G_N_ELEMENTS(hw_compat_s390x);

static void ccw_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    NMIClass *nc = NMI_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_CLASS(mc);
    DumpSKeysInterface *dsi = DUMP_SKEYS_INTERFACE_CLASS(oc);

    s390mc->max_threads = 1;
    s390mc->use_cpi = true;
    s390mc->use_certs = true;
    s390mc->use_secure = true;
    mc->reset = s390_machine_reset;
    mc->block_default_type = IF_VIRTIO;
    mc->no_cdrom = 1;
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->max_cpus = S390_MAX_CPUS;
    mc->has_hotpluggable_cpus = true;
    mc->smp_props.books_supported = true;
    mc->smp_props.drawers_supported = true;
    assert(!mc->get_hotplug_handler);
    mc->get_hotplug_handler = s390_get_hotplug_handler;
    mc->cpu_index_to_instance_props = s390_cpu_index_to_props;
    mc->possible_cpu_arch_ids = s390_possible_cpu_arch_ids;
    /* it is overridden with 'host' cpu *in kvm_arch_init* */
    mc->default_cpu_type = S390_CPU_TYPE_NAME("qemu");
    hc->pre_plug = s390_machine_device_pre_plug;
    hc->plug = s390_machine_device_plug;
    hc->unplug_request = s390_machine_device_unplug_request;
    hc->unplug = s390_machine_device_unplug;
    nc->raise_nmi = s390_nmi;
    mc->default_ram_id = "s390.ram";
    mc->default_nic = "virtio-net-ccw";
    dsi->qmp_dump_skeys = s390_qmp_dump_skeys;

    object_class_property_add_bool(oc, "aes-key-wrap",
                                   machine_get_aes_key_wrap,
                                   machine_set_aes_key_wrap);
    object_class_property_set_description(oc, "aes-key-wrap",
            "enable/disable AES key wrapping using the CPACF wrapping key");

    object_class_property_add_bool(oc, "ext-measurement-block",
                                   machine_get_ext_measurement_block,
                                   machine_set_ext_measurement_block);
    object_class_property_set_description(oc, "ext-measurement-block",
            "provide the extended-I/O-measurement-block facility of the "
            "channel subsystem (format-1 measurement blocks)");

    object_class_property_add_bool(oc, "esa390-cm",
                                   machine_get_esa390_cm,
                                   machine_set_esa390_cm);
    object_class_property_set_description(oc, "esa390-cm",
            "start the configuration in the ESA/390-compatibility mode; "
            "the guest switches to z/Architecture with SIGP set-architecture "
            "(KVM only)");

    object_class_property_add_bool(oc, "dea-key-wrap",
                                   machine_get_dea_key_wrap,
                                   machine_set_dea_key_wrap);
    object_class_property_set_description(oc, "dea-key-wrap",
            "enable/disable DEA key wrapping using the CPACF wrapping key");

    object_class_property_add(oc, "loadparm", "loadparm", &str_type_info,
                              machine_get_loadparm, machine_set_loadparm,
                              NULL, NULL);
    object_class_property_set_description(oc, "loadparm",
            "Up to 8 chars in set of [A-Za-z0-9. ] (lower case chars converted"
            " to upper case) to pass to machine loader, boot manager,"
            " and guest kernel");

    object_class_property_add(oc, "chsc-config-token", "str", &str_type_info,
                              machine_get_chsc_config_token,
                              machine_set_chsc_config_token, NULL, NULL);
    object_class_property_set_description(oc, "chsc-config-token",
            "64-byte CHSC configuration token as 128 hexadecimal digits");

    object_class_property_add(oc, "ipl", "str", &str_type_info,
                              machine_get_ipl, machine_set_ipl, NULL, NULL);
    object_class_property_set_description(oc, "ipl",
            "CCW device address to IPL from, as devno or cssid.ssid.devno");
    object_class_property_add(oc, "boot-certs", "BootCertificatesList",
                              &BootCertificatesList_type_info,
                              machine_get_boot_certs, machine_set_boot_certs, NULL, NULL);
    object_class_property_set_description(oc, "boot-certs",
            "provide paths to a directory and/or a certificate file for secure boot");

    object_class_property_add_bool(oc, "secure-boot",
                                   machine_get_secure_boot,
                                   machine_set_secure_boot);
    object_class_property_set_description(oc, "secure-boot",
            "enable/disable secure boot");

    object_class_property_add_bool(oc, "pv",
                                   machine_get_pv,
                                   NULL);
    object_class_property_set_description(oc, "pv",
            "true while the guest runs in protected virtualization mode (read-only)");
}

static inline void s390_machine_initfn(Object *obj)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    ms->aes_key_wrap = true;
    ms->dea_key_wrap = true;
    ms->ext_measurement_block = true;
    ms->ipl_notifier.notify = s390_machine_ipl_notify;
    qemu_add_machine_init_done_notifier(&ms->ipl_notifier);
}

static void s390_machine_finalize(Object *obj)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    qemu_remove_machine_init_done_notifier(&ms->ipl_notifier);
    object_unref(ms->ipl_device);
}

static const TypeInfo ccw_machine_info = {
    .name          = TYPE_S390_CCW_MACHINE,
    .parent        = TYPE_MACHINE,
    .abstract      = true,
    .instance_size = sizeof(S390CcwMachineState),
    .instance_init = s390_machine_initfn,
    .instance_finalize = s390_machine_finalize,
    .class_size = sizeof(S390CcwMachineClass),
    .class_init    = ccw_machine_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_NMI },
        { TYPE_HOTPLUG_HANDLER},
        { TYPE_DUMP_SKEYS_INTERFACE},
        { }
    },
};

#define DEFINE_CCW_MACHINE_IMPL(latest, ...)                                  \
    static void MACHINE_VER_SYM(mach_init, ccw, __VA_ARGS__)(MachineState *mach) \
    {                                                                         \
        MACHINE_VER_SYM(instance_options, ccw, __VA_ARGS__)(mach);            \
        ccw_init(mach);                                                       \
    }                                                                         \
    static void MACHINE_VER_SYM(class_init, ccw, __VA_ARGS__)(                \
        ObjectClass *oc,                                                      \
        const void *data)                                                     \
    {                                                                         \
        MachineClass *mc = MACHINE_CLASS(oc);                                 \
        /* Apply global s390x-wide default properties */                      \
        compat_props_add(mc->compat_props, hw_compat_s390x,                   \
                         hw_compat_s390x_len);                                \
        MACHINE_VER_SYM(class_options, ccw, __VA_ARGS__)(mc);                 \
        mc->desc = "Virtual s390x machine (version " MACHINE_VER_STR(__VA_ARGS__) ")"; \
        mc->init = MACHINE_VER_SYM(mach_init, ccw, __VA_ARGS__);              \
        MACHINE_VER_DEPRECATION(__VA_ARGS__);                                 \
        if (latest) {                                                         \
            mc->alias = "s390-ccw-virtio";                                    \
            mc->is_default = true;                                            \
        }                                                                     \
    }                                                                         \
    static const TypeInfo MACHINE_VER_SYM(info, ccw, __VA_ARGS__) =           \
    {                                                                         \
        .name = MACHINE_VER_TYPE_NAME("s390-ccw-virtio", __VA_ARGS__),        \
        .parent = TYPE_S390_CCW_MACHINE,                                      \
        .class_init = MACHINE_VER_SYM(class_init, ccw, __VA_ARGS__),          \
        .secure = true,                                                       \
    };                                                                        \
    static void MACHINE_VER_SYM(register, ccw, __VA_ARGS__)(void)             \
    {                                                                         \
        MACHINE_VER_DELETION(__VA_ARGS__);                                    \
        type_register_static(&MACHINE_VER_SYM(info, ccw, __VA_ARGS__));       \
    }                                                                         \
    type_init(MACHINE_VER_SYM(register, ccw, __VA_ARGS__))

#define DEFINE_CCW_MACHINE_AS_LATEST(major, minor) \
    DEFINE_CCW_MACHINE_IMPL(true, major, minor)

#define DEFINE_CCW_MACHINE(major, minor) \
    DEFINE_CCW_MACHINE_IMPL(false, major, minor)


static void ccw_machine_11_2_instance_options(MachineState *machine)
{
    /*
     * A CPU version of 0xff marks a z/VM virtual CPU.  KVM guests are not
     * running under z/VM, so expose the value used by current real hardware.
     */
    s390_set_kvm_cpu_version(0);
}

static void ccw_machine_11_2_class_options(MachineClass *mc)
{
}
DEFINE_CCW_MACHINE_AS_LATEST(11, 2);

/*
 * Mainframe machine types: host-mirrored LPAR personalities.
 *
 * TYPE_S390_MAINFRAME_MACHINE is the abstract base of the machine types
 * that present a guest as a logical partition of a real IBM machine rather
 * than as a generic virtual s390x machine (today "czam"; a sibling for
 * ESA/390-mode partitions of older machines can derive from the same base).
 *
 * These machines present a KVM guest as a logical partition of the IBM
 * machine type it physically runs on, instead of as a generic virtual
 * s390x machine.  Machine-level identity (type, model, manufacturer, plant
 * and the serial/sequence number) is the host's: under KVM, STIDP and
 * SYSIB 1.1.1/1.2.x already reflect the real machine, so the host serial is
 * reused as-is.  The partition identity in SYSIB 2.2.2 is replaced by the
 * configured virtual partition (default number 1, name QEMUKVM), and the
 * SCLP storage increment mirrors the host's memory block size, as reported
 * by Linux.  The host's machine type must match the personality.
 */

#define MAINFRAME_SYSINFO_PATH "/proc/sysinfo"
#define MAINFRAME_BLOCK_SIZE_PATH "/sys/devices/system/memory/block_size_bytes"
#define MAINFRAME_DEFAULT_LPAR_NAME "QEMUKVM"
#define MAINFRAME_DEFAULT_LPAR_NUMBER 1

static S390CcwMachineState *mainframe_machine(void)
{
    Object *obj = qdev_get_machine();
    S390CcwMachineState *ms;

    if (!obj || !object_dynamic_cast(obj, TYPE_S390_CCW_MACHINE)) {
        return NULL;
    }
    ms = S390_CCW_MACHINE(obj);
    return S390_CCW_MACHINE_GET_CLASS(ms)->mirror_type ? ms : NULL;
}

bool s390_mainframe_lpar_identity(uint16_t *number, uint8_t name_ebcdic[8])
{
    S390CcwMachineState *ms = mainframe_machine();

    if (!ms) {
        return false;
    }
    *number = ms->lpar_number;
    memset(name_ebcdic, 0x40, 8);
    ebcdic_put(name_ebcdic, ms->lpar_name, strlen(ms->lpar_name));
    return true;
}

uint64_t s390_mainframe_storage_increment(void)
{
    S390CcwMachineState *ms = mainframe_machine();

    return ms ? ms->storage_increment : 0;
}

/*
 * Guest-visible SCLP Read SCP Info and CHSC values of a z14 ZR1 logical
 * partition.  Under czam these replace the generic virtual-machine
 * values, so the guest sees the configuration and channel-subsystem
 * characteristics of a real LPAR.
 */
static void mainframe_set_bits(uint32_t *words, const uint8_t *bits, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        words[bits[i] / 32] |= cpu_to_be32(0x80000000U >> (bits[i] % 32));
    }
}

bool s390_mainframe_chsc_characteristics(uint32_t *general_chars,
                                   uint32_t *chsc_chars)
{
    /*
     * Facilities the emulated devices cannot back are deliberately absent:
     * extended I/O measurement words (50; no channel timings), adapter
     * interruptions and the QDIO assists (41, 56, 61, 66, 67; no QDIO
     * devices).  Dynamic I/O (12) and alternate SSI (108) are kept as on
     * the real machine.
     */
    static const uint8_t general[] = {
        0, 6, 7, 9, 10, 12, 13, 31, 38, 44, 45, 46, 48, 60, 74, 93, 108,
    };
    /* CHSC 0x0021 (Set Channel Subsystem Characteristics, 107) is absent. */
    static const uint8_t chsc[] = {
        0, 1, 2, 3, 4, 6, 7, 8, 10, 11, 64, 89, 102, 108, 109, 112, 126,
    };

    if (!mainframe_machine()) {
        return false;
    }
    mainframe_set_bits(general_chars, general, ARRAY_SIZE(general));
    if (!s390_css_ext_measurement_block()) {
        /* extended-I/O-measurement-block facility (48) */
        general_chars[1] &= ~cpu_to_be32(0x00008000);
    }
    mainframe_set_bits(chsc_chars, chsc, ARRAY_SIZE(chsc));
    return true;
}

void s390_mainframe_patch_read_info(uint8_t *ri)
{
    S390CcwMachineState *ms = mainframe_machine();
    /*
     * Byte 84 bit 0x04 is set on real LPAR hardware; its meaning is not
     * documented.
     */
    static const uint8_t conf_char[] = { 0x2e, 0x40, 0x93, 0xdf, 0xfd, 0xff };

    if (!ms) {
        return;
    }
    ri[11] = 0x04;
    ri[12] = 0x00;
    ri[13] = 0x01;
    ri[15] = 0x02;                                  /* MNEST */
    stl_be_p(ri + 44, 0xfcfcff77);
    stq_be_p(ri + 48, 0xe8060c4080000000ULL);       /* SCLP facilities */
    ri[67] = 0x01;
    memcpy(ri + 80, conf_char, sizeof(conf_char));  /* bytes 80-85 */
    stl_be_p(ri + 86, ms->host_capability);
    ri[91] = 0x75;
    ri[98] = 0xa3;
    stl_be_p(ri + 124, 0x0b000000);                 /* HMFAI */
}

/* Return the trimmed value of a "Key: value" line of /proc/sysinfo. */
static char *mainframe_sysinfo_value(const char *sysinfo, const char *key)
{
    g_auto(GStrv) lines = g_strsplit(sysinfo, "\n", -1);
    size_t keylen = strlen(key);

    for (int i = 0; lines[i]; i++) {
        if (!strncmp(lines[i], key, keylen) && lines[i][keylen] == ':') {
            return g_strstrip(g_strdup(lines[i] + keylen + 1));
        }
    }
    return NULL;
}

static void mainframe_check_host(S390CcwMachineState *ms, Error **errp)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    uint16_t mirror_type = S390_CCW_MACHINE_GET_CLASS(ms)->mirror_type;
    MachineState *machine = MACHINE(ms);
    g_autofree char *sysinfo = NULL;
    g_autofree char *type = NULL;
    g_autofree char *block = NULL;
    g_autoptr(GError) gerr = NULL;
    char expected[8];
    uint64_t increment;

    if (!kvm_enabled()) {
        error_setg(errp, "machine %s requires KVM", mc->name);
        return;
    }
    if (!g_file_get_contents(MAINFRAME_SYSINFO_PATH, &sysinfo, NULL, &gerr)) {
        error_setg(errp, "cannot read %s: %s", MAINFRAME_SYSINFO_PATH,
                   gerr->message);
        return;
    }
    type = mainframe_sysinfo_value(sysinfo, "Type");
    snprintf(expected, sizeof(expected), "%04x", mirror_type);
    if (!type || strcmp(type, expected)) {
        error_setg(errp, "machine %s mirrors a %s host, but the host "
                   "machine type is %s", mc->name, expected,
                   type ? type : "unknown");
        return;
    }

    {
        g_autofree char *cap = mainframe_sysinfo_value(sysinfo, "Capability");

        ms->host_capability = cap ? g_ascii_strtoull(cap, NULL, 10) : 0;
    }

    if (!g_file_get_contents(MAINFRAME_BLOCK_SIZE_PATH, &block, NULL, &gerr)) {
        error_setg(errp, "cannot read %s: %s", MAINFRAME_BLOCK_SIZE_PATH,
                   gerr->message);
        return;
    }
    increment = g_ascii_strtoull(g_strstrip(block), NULL, 16);
    if (increment < MiB || !is_power_of_2(increment)) {
        error_setg(errp, "unusable host storage increment 0x%" PRIx64,
                   increment);
        return;
    }
    if (machine->ram_size % increment) {
        error_setg(errp, "memory size must be a multiple of the host storage "
                   "increment (%" PRIu64 " MiB)", increment / MiB);
        return;
    }
    ms->storage_increment = increment;
}

/* czam v1: a z14 ZR1 (3907) LPAR, mirrored from the KVM host. */
static void czam_v1_init(MachineState *machine)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(machine);
    S390CPU *cpu;

    mainframe_check_host(ms, &error_fatal);
    ccw_machine_11_2_instance_options(machine);
    ccw_init(machine);

    cpu = S390_CPU(first_cpu);
    if (cpu && cpu->model && cpu->model->def &&
        cpu->model->def->type != 0x3907) {
        error_report("machine %s requires a 3907 CPU model "
                     "(e.g. -cpu host or z14ZR1), not %s",
                     MACHINE_GET_CLASS(machine)->name, cpu->model->def->name);
        exit(1);
    }
}

static char *mainframe_get_lpar_name(Object *obj, Error **errp)
{
    return g_strdup(S390_CCW_MACHINE(obj)->lpar_name);
}

static void mainframe_set_lpar_name(Object *obj, const char *value, Error **errp)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);
    size_t len = strlen(value);

    /* Partition names: 1-8 characters from A-Z, 0-9, #, $ and @. */
    if (!len || len > 8 || !g_ascii_isalpha(value[0]) ||
        strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$@") != len) {
        error_setg(errp, "lpar-name must be 1-8 characters A-Z, 0-9, #, $ "
                   "or @, starting with a letter");
        return;
    }
    g_free(ms->lpar_name);
    ms->lpar_name = g_strdup(value);
}

static void mainframe_get_lpar_number(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint16_t value = S390_CCW_MACHINE(obj)->lpar_number;

    visit_type_uint16(v, name, &value, errp);
}

static void mainframe_set_lpar_number(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint16_t value;

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }
    if (!value || value > 0xff) {
        error_setg(errp, "lpar-number must be between 1 and 255");
        return;
    }
    S390_CCW_MACHINE(obj)->lpar_number = value;
}

static void mainframe_instance_init(Object *obj)
{
    S390CcwMachineState *ms = S390_CCW_MACHINE(obj);

    ms->lpar_name = g_strdup(MAINFRAME_DEFAULT_LPAR_NAME);
    ms->lpar_number = MAINFRAME_DEFAULT_LPAR_NUMBER;
}

static void mainframe_instance_finalize(Object *obj)
{
    g_free(S390_CCW_MACHINE(obj)->lpar_name);
}

bool s390_mainframe_zos_compat(void)
{
    S390CcwMachineState *ms = mainframe_machine();

    return ms && S390_CCW_MACHINE_GET_CLASS(ms)->zos_compat;
}

static void mainframe_class_init(ObjectClass *oc, const void *data)
{
    object_class_property_add_str(oc, "lpar-name", mainframe_get_lpar_name,
                                  mainframe_set_lpar_name);
    object_class_property_set_description(oc, "lpar-name",
        "Partition name reported in SYSIB 2.2.2 (default QEMUKVM)");
    object_class_property_add(oc, "lpar-number", "uint16", &uint16_type_info,
                              mainframe_get_lpar_number,
                              mainframe_set_lpar_number, NULL, NULL);
    object_class_property_set_description(oc, "lpar-number",
        "Partition number reported in SYSIB 2.2.2 (default 1)");
}

/*
 * czam ("CZAM hardware mode", z/Architecture-only machines) is versioned
 * like QEMU's versioned machine types: czam-vN never changes once
 * released, and the "czam" alias names the newest version.
 *
 * czam versions are bumped only by explicit maintainer decision.  Do not
 * add a version for minor changes; fixes that keep the guest-visible
 * machine identical go into the existing version.
 *
 * Version 1 (formerly named "mtm-3907") is a z14 ZR1 (3907) LPAR
 * personality with the QEMU 11.2 machine compat properties.
 */
static void czam_v1_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_CLASS(mc);

    compat_props_add(mc->compat_props, hw_compat_s390x, hw_compat_s390x_len);
    ccw_machine_11_2_class_options(mc);
    mc->desc = "CZAM hardware mode";
    mc->init = czam_v1_init;
#ifdef CONFIG_KVM
    mc->default_cpu_type = S390_CPU_TYPE_NAME("host");
#else
    /*
     * The "host" CPU type exists only in KVM-enabled builds.  Default to
     * the matching model elsewhere so that selecting the machine fails
     * with "requires KVM" instead of tripping over a missing CPU class.
     */
    mc->default_cpu_type = S390_CPU_TYPE_NAME("z14ZR1");
#endif
    /* Operator messages on the first serial (e.g. stdio with -nographic). */
    s390mc->line_mode_console_first = true;
    /* One storage increment of a typical z14 LPAR (1 GiB memory blocks). */
    mc->default_ram_size = 1 * GiB;
    s390mc->mirror_type = 0x3907;
    s390mc->zos_compat = true;
    mc->alias = "czam";
}

static const TypeInfo mainframe_types[] = {
    {
        .name = TYPE_S390_MAINFRAME_MACHINE,
        .parent = TYPE_S390_CCW_MACHINE,
        .abstract = true,
        .instance_init = mainframe_instance_init,
        .instance_finalize = mainframe_instance_finalize,
        .class_init = mainframe_class_init,
    }, {
        .name = MACHINE_TYPE_NAME("czam-v1"),
        .parent = TYPE_S390_MAINFRAME_MACHINE,
        .class_init = czam_v1_class_init,
        .secure = true,
    },
};

DEFINE_TYPES(mainframe_types)

static void ccw_machine_11_1_instance_options(MachineState *machine)
{
    ccw_machine_11_2_instance_options(machine);
}

static void ccw_machine_11_1_class_options(MachineClass *mc)
{
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_CLASS(mc);
    static GlobalProperty compat[] = {
        { TYPE_S390_PCI_HOST_BRIDGE, "x-zpci-migr-enabled", "off" },
    };

    s390mc->use_certs = false;
    s390mc->use_secure = false;

    ccw_machine_11_2_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_11_1, hw_compat_11_1_len);
    compat_props_add(mc->compat_props, compat, G_N_ELEMENTS(compat));
}
DEFINE_CCW_MACHINE(11, 1);

static void ccw_machine_11_0_instance_options(MachineState *machine)
{
    ccw_machine_11_1_instance_options(machine);
    /* Preserve the CPU identification exposed by older machine types. */
    s390_set_kvm_cpu_version(0xff);
}

static void ccw_machine_11_0_class_options(MachineClass *mc)
{
    /*
     * Preserve v11.0 and older version behavior:
     * keep legacy virtio-pci enabled.
     */
    static GlobalProperty compat[] = {
        { TYPE_VIRTIO_PCI, "disable-legacy", "off", .optional = true },
    };
    ccw_machine_11_1_class_options(mc);
    compat_props_add(mc->compat_props, compat, G_N_ELEMENTS(compat));
    compat_props_add(mc->compat_props, hw_compat_11_0, hw_compat_11_0_len);
}
DEFINE_CCW_MACHINE(11, 0);

static void ccw_machine_10_2_instance_options(MachineState *machine)
{
    ccw_machine_11_0_instance_options(machine);
}

static void ccw_machine_10_2_class_options(MachineClass *mc)
{
    ccw_machine_11_0_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_10_2, hw_compat_10_2_len);
}
DEFINE_CCW_MACHINE(10, 2);

static void ccw_machine_10_1_instance_options(MachineState *machine)
{
    ccw_machine_10_2_instance_options(machine);
}

static void ccw_machine_10_1_class_options(MachineClass *mc)
{
    ccw_machine_10_2_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_10_1, hw_compat_10_1_len);
}
DEFINE_CCW_MACHINE(10, 1);

static void ccw_machine_10_0_instance_options(MachineState *machine)
{
    ccw_machine_10_1_instance_options(machine);
}

static void ccw_machine_10_0_class_options(MachineClass *mc)
{
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_CLASS(mc);
    s390mc->use_cpi = false;

    ccw_machine_10_1_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_10_0, hw_compat_10_0_len);
}
DEFINE_CCW_MACHINE(10, 0);

static void ccw_machine_9_2_instance_options(MachineState *machine)
{
    ccw_machine_10_0_instance_options(machine);
}

static void ccw_machine_9_2_class_options(MachineClass *mc)
{
    static GlobalProperty compat[] = {
        { TYPE_S390_PCI_DEVICE, "relaxed-translation", "off", },
    };

    ccw_machine_10_0_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_9_2, hw_compat_9_2_len);
    compat_props_add(mc->compat_props, compat, G_N_ELEMENTS(compat));
}
DEFINE_CCW_MACHINE(9, 2);

static void ccw_machine_9_1_instance_options(MachineState *machine)
{
    ccw_machine_9_2_instance_options(machine);
}

static void ccw_machine_9_1_class_options(MachineClass *mc)
{
    ccw_machine_9_2_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_9_1, hw_compat_9_1_len);
}
DEFINE_CCW_MACHINE(9, 1);

static void ccw_machine_9_0_instance_options(MachineState *machine)
{
    ccw_machine_9_1_instance_options(machine);
}

static void ccw_machine_9_0_class_options(MachineClass *mc)
{
    static GlobalProperty compat[] = {
        { TYPE_QEMU_S390_FLIC, "migrate-all-state", "off", },
    };

    ccw_machine_9_1_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_9_0, hw_compat_9_0_len);
    compat_props_add(mc->compat_props, compat, G_N_ELEMENTS(compat));
}
DEFINE_CCW_MACHINE(9, 0);

static void ccw_machine_8_2_instance_options(MachineState *machine)
{
    ccw_machine_9_0_instance_options(machine);
}

static void ccw_machine_8_2_class_options(MachineClass *mc)
{
    ccw_machine_9_0_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_8_2, hw_compat_8_2_len);
}
DEFINE_CCW_MACHINE(8, 2);

static void ccw_machine_8_1_instance_options(MachineState *machine)
{
    ccw_machine_8_2_instance_options(machine);
}

static void ccw_machine_8_1_class_options(MachineClass *mc)
{
    ccw_machine_8_2_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_8_1, hw_compat_8_1_len);
    mc->smp_props.drawers_supported = false;
    mc->smp_props.books_supported = false;
}
DEFINE_CCW_MACHINE(8, 1);

static void ccw_machine_8_0_instance_options(MachineState *machine)
{
    ccw_machine_8_1_instance_options(machine);
}

static void ccw_machine_8_0_class_options(MachineClass *mc)
{
    ccw_machine_8_1_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_8_0, hw_compat_8_0_len);
}
DEFINE_CCW_MACHINE(8, 0);

static void ccw_machine_7_2_instance_options(MachineState *machine)
{
    ccw_machine_8_0_instance_options(machine);
}

static void ccw_machine_7_2_class_options(MachineClass *mc)
{
    ccw_machine_8_0_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_7_2, hw_compat_7_2_len);
}
DEFINE_CCW_MACHINE(7, 2);

static void ccw_machine_7_1_instance_options(MachineState *machine)
{
    static const S390FeatInit qemu_cpu_feat = { S390_FEAT_LIST_QEMU_V7_1 };

    ccw_machine_7_2_instance_options(machine);
    s390_cpudef_featoff_greater(16, 1, S390_FEAT_PAIE);
    s390_set_qemu_cpu_model(0x8561, 15, 1, qemu_cpu_feat);
}

static void ccw_machine_7_1_class_options(MachineClass *mc)
{
    S390CcwMachineClass *s390mc = S390_CCW_MACHINE_CLASS(mc);
    static GlobalProperty compat[] = {
        { TYPE_S390_PCI_DEVICE, "interpret", "off", },
        { TYPE_S390_PCI_DEVICE, "forwarding-assist", "off", },
    };

    ccw_machine_7_2_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_7_1, hw_compat_7_1_len);
    compat_props_add(mc->compat_props, compat, G_N_ELEMENTS(compat));
    s390mc->max_threads = S390_MAX_CPUS;
}
DEFINE_CCW_MACHINE(7, 1);

static void ccw_machine_7_0_instance_options(MachineState *machine)
{
    static const S390FeatInit qemu_cpu_feat = { S390_FEAT_LIST_QEMU_V7_0 };

    ccw_machine_7_1_instance_options(machine);
    s390_set_qemu_cpu_model(0x8561, 15, 1, qemu_cpu_feat);
}

static void ccw_machine_7_0_class_options(MachineClass *mc)
{
    ccw_machine_7_1_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_7_0, hw_compat_7_0_len);
}
DEFINE_CCW_MACHINE(7, 0);

static void ccw_machine_6_2_instance_options(MachineState *machine)
{
    static const S390FeatInit qemu_cpu_feat = { S390_FEAT_LIST_QEMU_V6_2 };

    ccw_machine_7_0_instance_options(machine);
    s390_set_qemu_cpu_model(0x3906, 14, 2, qemu_cpu_feat);
}

static void ccw_machine_6_2_class_options(MachineClass *mc)
{
    ccw_machine_7_0_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_6_2, hw_compat_6_2_len);
}
DEFINE_CCW_MACHINE(6, 2);

static void ccw_machine_6_1_instance_options(MachineState *machine)
{
    ccw_machine_6_2_instance_options(machine);
    s390_cpudef_featoff_greater(16, 1, S390_FEAT_NNPA);
    s390_cpudef_featoff_greater(16, 1, S390_FEAT_VECTOR_PACKED_DECIMAL_ENH2);
    s390_cpudef_featoff_greater(16, 1, S390_FEAT_BEAR_ENH);
    s390_cpudef_featoff_greater(16, 1, S390_FEAT_RDP);
    s390_cpudef_featoff_greater(16, 1, S390_FEAT_PAI);
}

static void ccw_machine_6_1_class_options(MachineClass *mc)
{
    ccw_machine_6_2_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_6_1, hw_compat_6_1_len);
    mc->smp_props.prefer_sockets = true;
}
DEFINE_CCW_MACHINE(6, 1);

static void ccw_machine_6_0_instance_options(MachineState *machine)
{
    static const S390FeatInit qemu_cpu_feat = { S390_FEAT_LIST_QEMU_V6_0 };

    ccw_machine_6_1_instance_options(machine);
    s390_set_qemu_cpu_model(0x2964, 13, 2, qemu_cpu_feat);
}

static void ccw_machine_6_0_class_options(MachineClass *mc)
{
    ccw_machine_6_1_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_6_0, hw_compat_6_0_len);
}
DEFINE_CCW_MACHINE(6, 0);

static void ccw_machine_5_2_instance_options(MachineState *machine)
{
    ccw_machine_6_0_instance_options(machine);
}

static void ccw_machine_5_2_class_options(MachineClass *mc)
{
    ccw_machine_6_0_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_5_2, hw_compat_5_2_len);
}
DEFINE_CCW_MACHINE(5, 2);

static void ccw_machine_5_1_instance_options(MachineState *machine)
{
    ccw_machine_5_2_instance_options(machine);
}

static void ccw_machine_5_1_class_options(MachineClass *mc)
{
    ccw_machine_5_2_class_options(mc);
    compat_props_add(mc->compat_props, hw_compat_5_1, hw_compat_5_1_len);
}
DEFINE_CCW_MACHINE(5, 1);

static void ccw_machine_register_types(void)
{
    type_register_static(&ccw_machine_info);
}

type_init(ccw_machine_register_types)
