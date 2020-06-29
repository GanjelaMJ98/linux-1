#define DEBUG
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/nfc.h>
#include <linux/netdevice.h>
#include <net/nfc/nfc.h>

#include "pn533.h"

#define VERSION "0.1"

#define PN533_SPI_DRIVER_NAME "pn533_spi"
/*
    nfc_pins: nc_pins {
        brcm,pins = <17>;
        brcm,function = <0>;
    };
    pn533: pn533@0{
        compatible = "nxp,pn533-spi";
        reg = <0>;
        spi-max-frequency = <125000000>;
        interrupt-parent = <&gpio>;
        interrupts = <17 IRQ_TYPE_EDGE_FALLING>;
    };
*/

struct pn533_spi_phy {
    struct spi_device *spi_dev;
    struct pn533 *priv;

    bool aborted;

    int hard_fault;     /*
                 * < 0 if hardware error occurred (e.g. spi err)
                 * and prevents normal operation.
                 */
};

static int pn533_spi_send_ack(struct pn533 *dev, gfp_t flags)
{
    struct pn533_spi_phy *phy = dev->phy;
    struct spi_device *spi_dev = phy->spi_dev;

    static const u8 ack[7] = {0x01, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00};
    /* spec 6.2.1.3:  Preamble, SoPC (2), ACK Code (2), Postamble */
    int rc;

    rc = spi_write(spi_dev, ack, 7);

    printk("%s:%d -- %d\n",__func__,__LINE__,rc);

    return rc;
}


static int pn533_spi_send_frame(struct pn533 *dev,
                struct sk_buff *out)
{
    struct pn533_spi_phy *phy = dev->phy;
    struct spi_device *spi_dev = phy->spi_dev;
    int rc,i;
    u8 mas[10] = {0x01, 0x00, 0x00, 0xff, 0x02, 0xfe, 0xd4, 0x02, 0x2a, 0x00};;

    if (phy->hard_fault != 0)
        return phy->hard_fault;

    if (phy->priv == NULL)
        phy->priv = dev;

    phy->aborted = false;

    //mas[0] = 0x1;
    print_hex_dump_debug("PN533_SPI: ", DUMP_PREFIX_NONE, 16, 1,
                 mas, out->len+1, false);

    // for(i=0; i < out->len; i++);
    //     mas[1+i]=out->data[i];


    rc = spi_write(spi_dev, mas, out->len+1);

    printk("%s:%s:%d -- %d\n",__FILE__,__func__,__LINE__,rc);

    return rc;
}





static int pn533_spi_read(struct pn533_spi_phy *phy, struct sk_buff **skb)
{

    struct spi_device *spi_dev = phy->spi_dev;
    int len = PN533_EXT_FRAME_HEADER_LEN +
          PN533_STD_FRAME_MAX_PAYLOAD_LEN +
          PN533_STD_FRAME_TAIL_LEN + 1;
    int r;

    printk("===============pn533_spi_read==============\n");
    
    *skb = alloc_skb(len, GFP_KERNEL);
    if (*skb == NULL)
        return -ENOMEM;
    
    r = spi_read(spi_dev, skb_put(*skb, len), len);
    if (r != len) {
        //nfc_err(spi_dev->dev, "cannot read. r=%d len=%d\n", r, len);
        kfree_skb(*skb);
        return -EREMOTEIO;
    }

    printk("===============pn533_spi_read READ==============\n");

    if (!((*skb)->data[0] & 0x01)) {
        //nfc_err(spi_dev->dev, "READY flag not set");
        kfree_skb(*skb);
        return -EBUSY;
    }

    printk("===============pn533_spi_read KFREE==============\n");
    /* remove READY byte */
    skb_pull(*skb, 1);
    /* trim to frame size */
    skb_trim(*skb, phy->priv->ops->rx_frame_size((*skb)->data));

    printk("===============pn533_spi_read END==============\n");

    return 0;
}


static void pn533_spi_abort_cmd(struct pn533 *dev, gfp_t flags)
{
    struct pn533_spi_phy *phy = dev->phy;

    printk("===============pn533_spi_abort_cmd==============\n");

    phy->aborted = true;

    /* An ack will cancel the last issued command */
    pn533_spi_send_ack(dev, flags);

    printk("===============pn533_spi_abort_cmd SEND==============\n");

    /* schedule cmd_complete_work to finish current command execution */
    pn533_recv_frame(phy->priv, NULL, -ENOENT);

    printk("===============pn533_spi_abort_cmd END==============\n");
}





static struct pn533_phy_ops spi_phy_ops = {
    .send_frame = pn533_spi_send_frame,
    .send_ack = pn533_spi_send_ack,
    .abort_cmd = pn533_spi_abort_cmd,
};



static irqreturn_t pn533_spi_irq_thread_fn(int irq, void *data)
{
    struct pn533_spi_phy *phy = data;
    struct spi_device *spi_dev;
    struct sk_buff *skb = NULL;
    int r;

    printk("===============pn533_spi_irq_thread_fn 0==============\n");
    

    if (!phy || irq != phy->spi_dev->irq) {
        WARN_ON_ONCE(1);
        return IRQ_NONE;
    }
    spi_dev = phy->spi_dev;
    
    dev_dbg(&spi_dev->dev, "IRQ\n");
    printk("===============pn533_spi_irq_thread_fn 1==============\n");

    if (phy->hard_fault != 0)
        return IRQ_HANDLED;

    printk("===============%s 2==============\n",__func__);

    r = pn533_spi_read(phy, &skb);

    printk("===============pn533_spi_irq_thread_fn 3==============\n");

    if (r == -EREMOTEIO) {
        phy->hard_fault = r;
        // printk("===============pn533_spi_irq_thread_fn 3==============\n");
        pn533_recv_frame(phy->priv, NULL, -EREMOTEIO);
        // printk("===============pn533_spi_irq_thread_fn 5==============\n");

        return IRQ_HANDLED;
    } else if ((r == -ENOMEM) || (r == -EBADMSG) || (r == -EBUSY)) {
        // printk("===============pn533_spi_irq_thread_fn 6==============\n");
        return IRQ_HANDLED;
    }

    if (!phy->aborted)
        printk("===============pn533_spi_irq_thread_fn 7==============\n");
        pn533_recv_frame(phy->priv, skb, 0);

    printk("===============pn533_spi_irq_thread_fn END==============\n");

    return IRQ_HANDLED;
}


static int pn533_spi_probe(struct spi_device *spi)
{

    
    struct spi_device *spi_dev = spi;
	struct pn533 *priv;
    struct pn533_spi_phy *phy;
    int r = 0;

    //printk("===============spi_pn533_probe==============\n");

    if(!spi)	
        return -ENOMEM;

    dev_dbg(&spi_dev->dev, "%s\n", __func__);
    dev_dbg(&spi_dev->dev, "IRQ: %d\n", spi_dev->irq);

    //spi_dev->irq = 17;
    printk("===============spi_pn533_probe _irq %d==============\n",spi_dev->irq);

    phy = devm_kzalloc(&spi_dev->dev, sizeof(struct pn533_spi_phy),
               GFP_KERNEL);
    if (!phy)
        return -ENOMEM;

    phy->spi_dev = spi_dev;

    priv = pn53x_common_init(PN533_DEVICE_PN532,
                     PN533_PROTO_REQ_ACK_RESP,
                     phy, &spi_phy_ops, NULL,
                     &phy->spi_dev->dev);
    
    printk("===============spi_pn533_probe REGISTER==============\n");
    
    if (IS_ERR(priv)) {
        r = PTR_ERR(priv);
        return r;
    }

    phy->priv = priv;

    r = request_threaded_irq(spi_dev->irq, NULL, pn533_spi_irq_thread_fn,
                IRQF_TRIGGER_FALLING |
                IRQF_SHARED | IRQF_ONESHOT,
                PN533_SPI_DRIVER_NAME, phy);

    if (r < 0) {
        nfc_err(&spi_dev->dev, "Unable to register IRQ handler\n");
        goto irq_rqst_err;
    }
    printk("===============spi_pn533_probe REQ==============\n");

    r = pn533_finalize_setup(priv);
    printk("===============spi_pn533_probe r = %d==============\n", r);
    if (r)
        goto fn_setup_err;

    r = nfc_register_device(priv->nfc_dev);
    if (r)
        goto fn_setup_err;

    return r;
fn_setup_err:
    free_irq(spi_dev->irq, phy);

irq_rqst_err:
   nfc_free_device(priv->nfc_dev);
   //pn533_unregister_device(phy->priv);


    return r;

}

static int pn533_spi_remove(struct spi_device *spi)
{
    struct pn533_spi_phy *phy;

    printk("===============pn533_spi_remove==============\n");

    phy = devm_kzalloc(&spi->dev, sizeof(struct pn533_spi_phy),
               GFP_KERNEL);
    if (!phy)
        return -ENOMEM;

    phy->spi_dev = spi;

    dev_dbg(&spi->dev, "%s\n", __func__);

    free_irq(spi->irq, phy);

    pn53x_unregister_nfc(phy->priv);
    pn53x_common_clean(phy->priv);

    //pn533_unregister_device(phy->priv);
    printk("===============pn533_spi_remove END==============\n");

    return 0;
}



static const struct of_device_id of_pn533_spi_match[] = {
    { .compatible = "nxp,pn533-spi" },
    { .compatible = "nxp,pn532-spi" },
    {},
};

MODULE_DEVICE_TABLE(of, of_pn533_spi_match);



static struct spi_driver pn533_spi_driver = {
    .driver = {
        .name =         PN533_SPI_DRIVER_NAME,
        .owner =        THIS_MODULE,
        .of_match_table = of_match_ptr(of_pn533_spi_match),
     },
    .probe = pn533_spi_probe,
    .remove = pn533_spi_remove,
};

module_spi_driver(pn533_spi_driver);

MODULE_AUTHOR("Pavel Ganzhela <p.ganzhela@omprussia.ru>");
MODULE_DESCRIPTION("PN533 SPI driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");