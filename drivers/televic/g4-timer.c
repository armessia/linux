/*
 * Copyright (C) 2018-2019      Televic NV.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/of_address.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>

/* register offsets compared to base */
#define MXC_TCTL        0x00          /* Control Register */
#define MXC_TPRER       0x04          /* Prescaler Register */
#define MXC_TSTAT       0x08          /* Status Register */
#define MXC_IR          0x0c          /* Interrupt Register */
#define MXC_TCMP1       0x10          /* Output Compare Register 1 */
#define MXC_TCMP2       0x14          /* Output Compare Register 2 */
#define MXC_TCMP3       0x18          /* Output Compare Register 3 */
#define MXC_TCN         0x24          /* Counter register */

/* TCTL register values */
#define MXC_TCTL_TEN      (1 << 0)      /* Enable module */
#define MXC_TCTL_WAITEN   (1 << 3)      /* Wait enable mode */
#define MXC_TCTL_CLK_PER  (1 << 6)      /* Select High Frequency Reference Clock */
#define MXC_TCTL_SWR      (1 << 15)     /* Software reset */
#define MXC_TCTL_OM1      (1 << 20)     /* Toggle on GPT_COMPARE1 signal. For debugging. */
#define MXC_TCTL_OM2      (1 << 23)     /* Toggle on GPT_COMPARE2 signal. For debugging. */
#define MXC_TCTL_OM3      (1 << 26)     /* Toggle on GPT_COMPARE3 signal. For debugging. */

#define MXC_OF2      (1 << 1)      /* Output Compare 2 */

#define TIMER_MAX              (7500-1)       ///< Maximum timer value. Equal to the frame period (5 msec) multiplied by the timer clock frequency (1.500 MHz) minus one.

struct g4_timer_dev {

	/* clocks */
	struct clk *clk_ipg; /* hardware clock signal */
	struct clk *clk_per; /* hardware clock signal */

	/* mapped address ranges */
	void __iomem *base;

	/* interrupts */
	int irq;

	/* tasklets */
	struct tasklet_struct tasklet;

	/* locks */
	spinlock_t gpio_spin_lock;

	/* irq_gpio */
	struct gpio_desc *irq_gpio_pin;
	unsigned int irq_gpio_on;  /* is the output enabled? */

	/* tasklet_gpio */
	struct gpio_desc *tasklet_gpio_pin;
	unsigned int tasklet_gpio_on;  /* is the output enabled? */
};

static inline void tcs_g4_timer_set_irq(struct g4_timer_dev *pdata)
{
	/* read irq settings */
	u32 irq_status = readl(pdata->base + MXC_IR);

	/* enable flags for callbacks */
	irq_status |= MXC_OF2;

	/* write irq settings */
	writel(irq_status, pdata->base + MXC_IR);
}

static irqreturn_t g4_timer_isr(int irq, void *data)
{
	unsigned long flags;
	struct g4_timer_dev* pdata = data;

	/* clear interrupts */
	writel((MXC_OF2), pdata->base + MXC_TSTAT);

	/* Toggle GPIO */
	spin_lock_irqsave(&pdata->gpio_spin_lock, flags);
	if (pdata->irq_gpio_pin) {
		pdata->irq_gpio_on = !pdata->irq_gpio_on;
		gpiod_set_value(pdata->irq_gpio_pin, pdata->irq_gpio_on);
	}
	spin_unlock_irqrestore(&pdata->gpio_spin_lock, flags);

	tasklet_hi_schedule(&pdata->tasklet);

	return IRQ_HANDLED;
}

static void g4_timer_tasklet(unsigned long data)
{
	unsigned long flags;
	struct g4_timer_dev* pdata = (struct g4_timer_dev *)data;

	/* Toggle GPIO */
	spin_lock_irqsave(&pdata->gpio_spin_lock, flags);
	if (pdata->tasklet_gpio_pin) {
		pdata->tasklet_gpio_on = !pdata->tasklet_gpio_on;
		gpiod_set_value(pdata->tasklet_gpio_pin, pdata->tasklet_gpio_on);
	}
	spin_unlock_irqrestore(&pdata->gpio_spin_lock, flags);
}

static const struct of_device_id g4_timer_ids[] = {
	{ .compatible = "g4-timer-main" },
	{ /* last entry must be empty */ }
};
MODULE_DEVICE_TABLE(of, g4_timer_ids);

static int g4_timer_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *np = pdev->dev.of_node;
	struct g4_timer_dev* pdata;

	pdata = devm_kzalloc(&pdev->dev, sizeof(struct g4_timer_dev), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	/* store pointer to platform data in the platform device */
	platform_set_drvdata(pdev, pdata);

	/* map the base address */
	pdata->base = of_iomap(np, 0);
	if (!pdata->base) {
		dev_err(&pdev->dev, "Failed to map region\n");
		return -ENOMEM;
	}

	/* get irq info */
	pdata->irq = platform_get_irq(pdev, 0);

	/* request irq */
	ret = devm_request_irq(&pdev->dev, pdata->irq, g4_timer_isr, 0, "g4-timer", pdata);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register g4-timer interrupt\n");
		goto ERROR_PROBE;
	}

	/* get ipg clock */
	pdata->clk_ipg = devm_clk_get(&pdev->dev, "ipg");
	if (IS_ERR(pdata->clk_ipg)) {
		dev_err(&pdev->dev, "Failed to get ipg clock resource\n");
		ret = PTR_ERR(pdata->clk_ipg);
		goto ERROR_PROBE;
	}

	/* prepare ipg clock */
	ret = clk_prepare_enable(pdata->clk_ipg);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare the ipg clock\n");
		goto ERROR_PROBE;
	}

	/* get par clock */
	pdata->clk_per = devm_clk_get(&pdev->dev, "per");
	if (IS_ERR(pdata->clk_per)) {
		dev_err(&pdev->dev, "Failed to get per clock resource\n");
		ret = PTR_ERR(pdata->clk_per);
		goto ERROR_PROBE;
	}

	/* prepare per clock */
	ret = clk_prepare_enable(pdata->clk_per);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare the per clock\n");
		goto ERROR_PROBE;
	}

	spin_lock_init(&pdata->gpio_spin_lock);

	tasklet_init(&pdata->tasklet, g4_timer_tasklet, (unsigned long)pdata);

	pdata->irq_gpio_pin = devm_gpiod_get_optional(&pdev->dev, "irq", GPIOD_OUT_HIGH);
	if (IS_ERR(pdata->irq_gpio_pin)) {
		dev_err(&pdev->dev, "Failed to request irq gpio\n");
		goto ERROR_PROBE;
	}

	pdata->tasklet_gpio_pin = devm_gpiod_get_optional(&pdev->dev, "tasklet", GPIOD_OUT_HIGH);
	if (IS_ERR(pdata->tasklet_gpio_pin)) {
		dev_err(&pdev->dev, "Failed to request tasklet gpio\n");
		goto ERROR_PROBE;
	}

	/* reset control */
	writel_relaxed(0, pdata->base + MXC_TCTL);

	/* reset prescaler */
	writel_relaxed(0, pdata->base + MXC_TPRER);

	/* set control register */
	/* Televic/HAV: apparently, the timer isn't clocked correctly if the WAITEN bit is not set.
	 * It isn't understood why this is the case. */
	writel_relaxed(MXC_TCTL_OM3	    | /* toggle compare channel 3 output pin */
	               MXC_TCTL_OM2	    | /* toggle compare channel 2 output pin */
	               MXC_TCTL_OM1	    | /* toggle compare channel 1 output pin */
	               MXC_TCTL_CLK_PER | /* clock source selection - peripheral clock */
	               MXC_TCTL_WAITEN  | /* enable gpt during wait mode */
	               MXC_TCTL_TEN,      /* enable gpt */
	               pdata->base + MXC_TCTL);

	/* write max value of the counter -> 5 ms */
	writel(TIMER_MAX, pdata->base + MXC_TCMP1);
	writel(0, pdata->base + MXC_TCMP2);
	writel(0, pdata->base + MXC_TCMP3);

	/* set prescaler */
	/* Televic/HAV: divide by 8 to go from 12.000 MHz to 1.500 MHz */
	writel_relaxed(7, pdata->base + MXC_TPRER);

	tcs_g4_timer_set_irq(pdata);

	dev_info(&pdev->dev, "g4_timer probed\n");

	return 0;

 ERROR_PROBE:

	if (pdata->base)
		iounmap(pdata->base);

	return 1;
}

static int g4_timer_remove(struct platform_device *pdev)
{
	struct g4_timer_dev *pdata = platform_get_drvdata(pdev);

	tasklet_kill(&pdata->tasklet);

	/* reset & disable the device */
	writel_relaxed(MXC_TCTL_SWR, pdata->base + MXC_TCTL);

	/* unmap io regions */
	iounmap(pdata->base);

	/* remove pointer to platform data in the platform device */
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver g4_timer_driver = {
	.probe		= g4_timer_probe,
	.remove		= g4_timer_remove,
	.driver		= {
		.name	= "g4_timer",
		.of_match_table = g4_timer_ids,
	},
};

static int __init g4_timer_init(void)
{
	return platform_driver_register(&g4_timer_driver);
}

static void __exit g4_timer_exit(void)
{
	platform_driver_unregister(&g4_timer_driver);
}

module_init(g4_timer_init);
module_exit(g4_timer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Televic NV.");
MODULE_DESCRIPTION("Confidea G4 - timer module");
