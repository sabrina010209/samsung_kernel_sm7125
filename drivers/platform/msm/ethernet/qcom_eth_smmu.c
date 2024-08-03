/* Copyright (c) 2020 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/of.h>

#include <linux/iommu.h>
#include <asm/dma-iommu.h>
#include <linux/platform_device.h>

#include <linux/qcom_eth_smmu.h>

static int (*vendor_probe_real)(struct pci_dev *, const struct pci_device_id *);
static void (*vendor_remove_real)(struct pci_dev *);

static int qcom_parse_smmu_attr(struct device *dev,
				    struct iommu_domain *domain,
				    const char *key,
				    enum iommu_attr attr)
{
	int rc = 0;
	unsigned int data = 1;

	if (of_find_property(dev->of_node, key, NULL)) {
		rc = iommu_domain_set_attr(domain, attr, &data);
		if (!rc)
			dev_dbg(dev, "enabled SMMU attribute %u\n", attr);
		else
			dev_err(dev, "failed to set SMMU attribute %u\n", attr);
	}

	return rc;
}

static int qcom_parse_smmu_attrs(struct device *dev,
				     struct iommu_domain *domain)
{
	int rc = 0;

	rc |= qcom_parse_smmu_attr(dev, domain,
		"qcom,smmu-attr-s1-bypass", DOMAIN_ATTR_S1_BYPASS);

	rc |= qcom_parse_smmu_attr(dev, domain,
		"qcom,smmu-attr-fastmap", DOMAIN_ATTR_FAST);

	rc |= qcom_parse_smmu_attr(dev, domain,
		"qcom,smmu-attr-atomic", DOMAIN_ATTR_ATOMIC);

	rc |= qcom_parse_smmu_attr(dev, domain,
		"qcom,smmu-attr-pt-coherent",
		DOMAIN_ATTR_PAGE_TABLE_IS_COHERENT);

	rc |= qcom_parse_smmu_attr(dev, domain,
		"qcom,smmu-attr-pt-coherent-force",
		DOMAIN_ATTR_PAGE_TABLE_FORCE_COHERENT);

	return rc;
}

static int __qcom_attach_smmu(struct device *dev)
{
	int rc;
	const char *key;
	u64 val64;

	dma_addr_t iova_base = 0;
	u64 iova_size = 0;

	struct dma_iommu_mapping *mapping = NULL;

	key = "qcom,smmu-iova-base";
	rc = of_property_read_u64(dev->of_node, key, &val64);
	if (rc) {
		dev_err(dev, "error parsing DT prop %s: %d\n", key, rc);
		return rc;
	}

	iova_base = (dma_addr_t)val64;

	key = "qcom,smmu-iova-size";
	rc = of_property_read_u64(dev->of_node, key, &val64);
	if (rc) {
		dev_err(dev, "error parsing DT prop %s: %d\n", key, rc);
		return rc;
	}

	iova_size = val64;

	dev_dbg(dev, "creating SMMU mapping with base=0x%llx, size=0x%llx\n",
		iova_base, iova_size);

	mapping = arm_iommu_create_mapping(dev->bus, iova_base, iova_size);
	if (IS_ERR(mapping)) {
		dev_err(dev, "failed to create SMMU mapping\n");
		return PTR_ERR(mapping);
	}

	rc = qcom_parse_smmu_attrs(dev, mapping->domain);
	if (rc) {
		dev_err(dev, "error parsing SMMU DT attributes\n");
		goto err_release_mapping;
	}

	rc = arm_iommu_attach_device(dev, mapping);
	if (rc) {
		dev_err(dev, "failed to attach device to smmu\n");
		goto err_release_mapping;
	}

	return 0;

err_release_mapping:
	arm_iommu_release_mapping(mapping);
	return rc;
}

static int qcom_attach_smmu(struct device *dev)
{
	bool dt_present = !!of_find_property(dev->of_node, "qcom,smmu", NULL);
	bool smmu_attached = !!iommu_get_domain_for_dev(dev);

	if (smmu_attached) {
		/* On platforms where IOMMU is attached automatically, we do
		 * not expect qcom,smmu property to be present in devicetree.
		 */
		if (dt_present) {
			dev_err(dev, "SMMU DT node is not expected\n");
			return -EEXIST;
		}

		return 0;
	}

	if (!dt_present) {
		dev_err(dev, "SMMU DT is required for the device\n");
		return -EFAULT;
	}

	return __qcom_attach_smmu(dev);
}

static void qcom_detach_smmu(struct device *dev)
{
	bool dt_present = !!of_find_property(dev->of_node, "qcom,smmu", NULL);
	bool smmu_attached = !!iommu_get_domain_for_dev(dev);

	/* Perform a manual deattach only if we were tasked with doing the
	 * attach originally.
	 */
	if (dt_present && smmu_attached) {
		struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);

		if (!mapping) {
			dev_err(dev, "Failed to retrieve IOMMU mapping\n");
			return;
		}

		arm_iommu_detach_device(dev);
		arm_iommu_release_mapping(mapping);
	}
}

static int vendor_qcom_probe(
		struct pci_dev *pdev,
		const struct pci_device_id *id)
{
	int rc;

	rc = qcom_attach_smmu(&pdev->dev);
	if (rc)
		return rc;

	rc = vendor_probe_real(pdev, id);
	if (rc) {
		qcom_detach_smmu(&pdev->dev);
		return rc;
	}

	return 0;
}

static void vendor_qcom_remove(struct pci_dev *pdev)
{
	vendor_remove_real(pdev);
	qcom_detach_smmu(&pdev->dev);
}

static int __vendor_qcom_register(struct pci_driver *pdrv)
{
	if (vendor_probe_real || vendor_remove_real) {
		pr_warn("%s: Driver already registered\n", __func__);
		return -EEXIST;
	}

	vendor_probe_real = pdrv->probe;
	pdrv->probe = vendor_qcom_probe;

	vendor_remove_real = pdrv->remove;
	pdrv->remove = vendor_qcom_remove;

	return 0;
}

static void __vendor_qcom_unregister(struct pci_driver *pdrv)
{
	if (vendor_probe_real) {
		pdrv->probe = vendor_probe_real;
		vendor_probe_real = NULL;
	}

	if (vendor_remove_real) {
		pdrv->remove = vendor_remove_real;
		vendor_remove_real = NULL;
	}
}

int qcom_smmu_register(struct pci_driver *pdrv)
{
	int rc;

	rc = __vendor_qcom_register(pdrv);
	if (rc)
		return rc;


	return 0;
}
EXPORT_SYMBOL(qcom_smmu_register);

void qcom_smmu_unregister(struct pci_driver *pdrv)
{
	__vendor_qcom_unregister(pdrv);
}
EXPORT_SYMBOL(qcom_smmu_unregister);

