// rtl8192cd_test.c - Sysfs para leer/escribir registros RF/BB del 8192cd con salvaguardas
// Compatible con kernels ~3.18 (OpenWrt vendeado)
//
// Compilar out-of-tree:
//   obj-m += rtl8192cd_test.o
//   EXTRA_CFLAGS += -I$(PWD)/package/kernel/rtl8192cd
//
// Author: tu_nombre
// License: GPL

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h> // init_net
#include <linux/if.h>          // IFNAMSIZ
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/types.h> // bool

// Header "umbrella" del driver Realtek en tu tree:
#include "8192cd_headers.h"
// Si tu drop realmente necesita otros headers, añádelos aquí:
// #include "8192cd_hw.h"

// Registros sensibles (direcciones BB/RF usadas como ejemplo)
#define REG_CR 0x0100   // Control Register (TX/RX enable) - BB
#define TXAGC_A 0x0c90  // TX Gain Control                - BB
#define RF_SYNTH 0x000f // RF Synthesizer Control         - RF

static struct kobject *rtl_test_kobj;
static DEFINE_MUTEX(rtl_test_lock);
static char interface_name[IFNAMSIZ] = "RTKWifi0"; // Interfaz por defecto

// ---------- Helpers: obtener priv desde una net_device ----------
static struct rtl8192cd_priv *get_priv(void)
{
    struct net_device *dev;
    struct rtl8192cd_priv *priv;

    dev = dev_get_by_name(&init_net, interface_name);
    if (!dev)
    {
        pr_err("rtl8192cd_test: No se encontró la interfaz %s\n", interface_name);
        return NULL;
    }
    priv = (struct rtl8192cd_priv *)netdev_priv(dev);
    dev_put(dev);
    return priv;
}

// ---------- Accesos BB/RF reales (vía helpers del driver) ----------
static int my_write_bb_reg(struct rtl8192cd_priv *priv, unsigned int addr,
                           unsigned int mask, unsigned int val)
{
    unsigned int old_val = RTL_R32(priv, addr);
    unsigned int new_val = (old_val & ~mask) | (val & mask);
    RTL_W32(priv, addr, new_val);
    pr_info("rtl8192cd_test: BB write addr=0x%04x mask=0x%08x val=0x%08x if=%s\n",
            addr, mask, new_val, interface_name);
    return 0;
}

static int my_write_rf_reg(struct rtl8192cd_priv *priv, unsigned int reg_addr,
                           unsigned int val)
{
    // BitMask 0xFFFFF: 20 bits RF (ajusta si tu chip usa otra máscara)
    PHY_SetRFReg(priv, RF92CD_PATH_A, reg_addr, 0xFFFFF, val);
    pr_info("rtl8192cd_test: RF write reg=0x%02x val=0x%05x if=%s\n",
            reg_addr, val, interface_name);
    return 0;
}

static int my_read_bb_reg(struct rtl8192cd_priv *priv, unsigned int addr,
                          unsigned int *out)
{
    *out = RTL_R32(priv, addr);
    pr_info("rtl8192cd_test: BB read addr=0x%04x -> 0x%08x if=%s\n",
            addr, *out, interface_name);
    return 0;
}

static int my_read_rf_reg(struct rtl8192cd_priv *priv, unsigned int reg_addr,
                          unsigned int *out)
{
    // Último parámetro suele ser bPseudoTest o similar; 1 está bien
    *out = PHY_QueryRFReg(priv, RF92CD_PATH_A, reg_addr, 0xFFFFF, 1);
    pr_info("rtl8192cd_test: RF read reg=0x%02x -> 0x%05x if=%s\n",
            reg_addr, *out, interface_name);
    return 0;
}

// ---------- Salvaguardas de registros ----------
static bool is_sensitive_reg(unsigned int addr, unsigned int *val)
{
    switch (addr)
    {
    case TXAGC_A:
        if (*val > 0xff)
        {
            pr_warn("rtl8192cd_test: TXAGC_A 0x%04x = 0x%x > 0xFF; forzando 0xff\n",
                    addr, *val);
            *val = 0xff; // clamp 6 bits
        }
        return false; // permitir escritura con valor ajustado

    case REG_CR:
    case RF_SYNTH:
        pr_warn("rtl8192cd_test: Acceso bloqueado a reg sensible 0x%04x\n", addr);
        return true; // bloquear completamente

    default:
        return false;
    }
}

// ---------- Sysfs: interface ----------
static ssize_t interface_store(struct kobject *kobj,
                               struct kobj_attribute *attr,
                               const char *buf, size_t count)
{
    char tmp[IFNAMSIZ];

    if (count >= IFNAMSIZ)
    {
        pr_err("rtl8192cd_test: Nombre de interfaz demasiado largo\n");
        return -EINVAL;
    }
    if (sscanf(buf, "%s", tmp) != 1)
        return -EINVAL;

    mutex_lock(&rtl_test_lock);
    strlcpy(interface_name, tmp, IFNAMSIZ);
    pr_info("rtl8192cd_test: Interfaz configurada a %s\n", interface_name);
    mutex_unlock(&rtl_test_lock);
    return count;
}

static ssize_t interface_show(struct kobject *kobj,
                              struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%s\n", interface_name);
}

static struct kobj_attribute interface_attr =
    __ATTR(interface, 0644, interface_show, interface_store);

// ---------- Sysfs: RF write (echo "addr value") ----------
static ssize_t rf_write_store(struct kobject *kobj,
                              struct kobj_attribute *attr,
                              const char *buf, size_t count)
{
    unsigned int addr, val;
    struct rtl8192cd_priv *priv = get_priv();
    if (!priv)
        return -ENODEV;

    if (sscanf(buf, "%x %x", &addr, &val) != 2)
        return -EINVAL;

    mutex_lock(&rtl_test_lock);
    if (is_sensitive_reg(addr, &val))
    {
        mutex_unlock(&rtl_test_lock);
        return -EPERM;
    }
    if (my_write_rf_reg(priv, addr, val))
    {
        mutex_unlock(&rtl_test_lock);
        return -EIO;
    }
    mutex_unlock(&rtl_test_lock);
    return count;
}

static struct kobj_attribute rf_write_attr =
    __ATTR(rf_write, 0200, NULL, rf_write_store);

// ---------- Sysfs: BB write (echo "addr mask value") ----------
static ssize_t bb_write_store(struct kobject *kobj,
                              struct kobj_attribute *attr,
                              const char *buf, size_t count)
{
    unsigned int addr, mask, val;
    struct rtl8192cd_priv *priv = get_priv();
    if (!priv)
        return -ENODEV;

    if (sscanf(buf, "%x %x %x", &addr, &mask, &val) != 3)
        return -EINVAL;

    mutex_lock(&rtl_test_lock);
    if (is_sensitive_reg(addr, &val))
    {
        mutex_unlock(&rtl_test_lock);
        return -EPERM;
    }
    if (my_write_bb_reg(priv, addr, mask, val))
    {
        mutex_unlock(&rtl_test_lock);
        return -EIO;
    }
    mutex_unlock(&rtl_test_lock);
    return count;
}

static struct kobj_attribute bb_write_attr =
    __ATTR(bb_write, 0200, NULL, bb_write_store);

// ---------- Sysfs: RF read (usa rf_read_params para setear addr) ----------
static unsigned int rf_read_addr;

static ssize_t rf_read_params_store(struct kobject *kobj,
                                    struct kobj_attribute *attr,
                                    const char *buf, size_t count)
{
    if (sscanf(buf, "%x", &rf_read_addr) != 1)
        return -EINVAL;
    return count;
}

static struct kobj_attribute rf_read_params_attr =
    __ATTR(rf_read_params, 0200, NULL, rf_read_params_store);

static ssize_t rf_read_show(struct kobject *kobj,
                            struct kobj_attribute *attr, char *buf)
{
    unsigned int val;
    struct rtl8192cd_priv *priv = get_priv();
    if (!priv)
        return -ENODEV;

    mutex_lock(&rtl_test_lock);
    if (my_read_rf_reg(priv, rf_read_addr, &val))
    {
        mutex_unlock(&rtl_test_lock);
        return -EIO;
    }
    mutex_unlock(&rtl_test_lock);
    return sprintf(buf, "0x%08x\n", val);
}

static struct kobj_attribute rf_read_attr =
    __ATTR(rf_read, 0444, rf_read_show, NULL);

// ---------- Sysfs: BB read (usa bb_read_params para setear addr) ----------
static unsigned int bb_read_addr;

static ssize_t bb_read_params_store(struct kobject *kobj,
                                    struct kobj_attribute *attr,
                                    const char *buf, size_t count)
{
    if (sscanf(buf, "%x", &bb_read_addr) != 1)
        return -EINVAL;
    return count;
}

static struct kobj_attribute bb_read_params_attr =
    __ATTR(bb_read_params, 0200, NULL, bb_read_params_store);

static ssize_t bb_read_show(struct kobject *kobj,
                            struct kobj_attribute *attr, char *buf)
{
    unsigned int val;
    struct rtl8192cd_priv *priv = get_priv();
    if (!priv)
        return -ENODEV;

    mutex_lock(&rtl_test_lock);
    if (my_read_bb_reg(priv, bb_read_addr, &val))
    {
        mutex_unlock(&rtl_test_lock);
        return -EIO;
    }
    mutex_unlock(&rtl_test_lock);
    return sprintf(buf, "0x%08x\n", val);
}

static struct kobj_attribute bb_read_attr =
    __ATTR(bb_read, 0444, bb_read_show, NULL);

// ---------- Sysfs: TX power quick read (lee TXAGC_A) ----------
static ssize_t tx_power_read_show(struct kobject *kobj,
                                  struct kobj_attribute *attr, char *buf)
{
    unsigned int val;
    struct rtl8192cd_priv *priv = get_priv();
    if (!priv)
        return -ENODEV;

    mutex_lock(&rtl_test_lock);
    if (my_read_bb_reg(priv, TXAGC_A, &val))
    {
        mutex_unlock(&rtl_test_lock);
        return -EIO;
    }
    mutex_unlock(&rtl_test_lock);
    return sprintf(buf, "TXAGC_A (0x%04x): 0x%08x (max estándar: 0x3F)\n",
                   TXAGC_A, val);
}

static struct kobj_attribute tx_power_read_attr =
    __ATTR(tx_power_read, 0444, tx_power_read_show, NULL);

// ---------- Init / Exit ----------
static int __init rtl_test_init(void)
{
    int ret = 0;

    rtl_test_kobj = kobject_create_and_add("rtl8192cd_test", kernel_kobj);
    if (!rtl_test_kobj)
    {
        pr_err("rtl8192cd_test: Falló la creación de kobject\n");
        return -ENOMEM;
    }

    ret |= sysfs_create_file(rtl_test_kobj, &interface_attr.attr);
    ret |= sysfs_create_file(rtl_test_kobj, &rf_write_attr.attr);
    ret |= sysfs_create_file(rtl_test_kobj, &bb_write_attr.attr);
    ret |= sysfs_create_file(rtl_test_kobj, &rf_read_attr.attr);
    ret |= sysfs_create_file(rtl_test_kobj, &rf_read_params_attr.attr);
    ret |= sysfs_create_file(rtl_test_kobj, &bb_read_attr.attr);
    ret |= sysfs_create_file(rtl_test_kobj, &bb_read_params_attr.attr);
    ret |= sysfs_create_file(rtl_test_kobj, &tx_power_read_attr.attr);

    if (ret)
    {
        pr_err("rtl8192cd_test: Falló la creación de archivos sysfs\n");
        kobject_put(rtl_test_kobj);
        return ret;
    }

    pr_info("rtl8192cd_test: sysfs listo, interfaz por defecto: %s\n", interface_name);
    return 0;
}

static void __exit rtl_test_exit(void)
{
    if (rtl_test_kobj)
    {
        sysfs_remove_file(rtl_test_kobj, &interface_attr.attr);
        sysfs_remove_file(rtl_test_kobj, &rf_write_attr.attr);
        sysfs_remove_file(rtl_test_kobj, &bb_write_attr.attr);
        sysfs_remove_file(rtl_test_kobj, &rf_read_attr.attr);
        sysfs_remove_file(rtl_test_kobj, &rf_read_params_attr.attr);
        sysfs_remove_file(rtl_test_kobj, &bb_read_attr.attr);
        sysfs_remove_file(rtl_test_kobj, &bb_read_params_attr.attr);
        sysfs_remove_file(rtl_test_kobj, &tx_power_read_attr.attr);
        kobject_put(rtl_test_kobj);
    }
    pr_info("rtl8192cd_test: sysfs descargado\n");
}

module_init(rtl_test_init);
module_exit(rtl_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tu_nombre");
MODULE_DESCRIPTION("Sysfs interface for safe RF/BB register access on RTL8192CD with dynamic interface");
