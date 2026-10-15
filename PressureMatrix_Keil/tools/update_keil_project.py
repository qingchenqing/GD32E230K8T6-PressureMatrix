"""Set the copied GigaDevice Keil template to this board and its local sources."""

from pathlib import Path
import xml.etree.ElementTree as ET


PROJECT = Path(__file__).resolve().parents[1] / "Keil" / "PressureMatrix.uvprojx"


def set_text(parent, name, value):
    item = parent.find(".//" + name)
    if item is None:
        raise ValueError(f"Missing Keil XML field: {name}")
    item.text = value


def add_group(groups, name, files):
    group = ET.SubElement(groups, "Group")
    ET.SubElement(group, "GroupName").text = name
    file_items = ET.SubElement(group, "Files")
    for path, file_type in files:
        item = ET.SubElement(file_items, "File")
        ET.SubElement(item, "FileName").text = Path(path).name
        ET.SubElement(item, "FileType").text = str(file_type)
        ET.SubElement(item, "FilePath").text = path


ET.register_namespace("xsi", "http://www.w3.org/2001/XMLSchema-instance")
tree = ET.parse(PROJECT)
root = tree.getroot()
target = root.find("./Targets/Target")
if target is None:
    raise ValueError("Keil target missing")

set_text(target, "TargetName", "GD32E230K8T6_PressureMatrix")
set_text(target, "Device", "GD32E230K8")
set_text(target, "PackID", "GigaDevice.GD32E23x_DFP.1.0.2")
set_text(target, "Cpu", 'IRAM(0x20000000,0x0002000) IROM(0x08000000,0x0010000) CPUTYPE("Cortex-M23") CLOCK(72000000) ELITTLE')
set_text(target, "OutputName", "PressureMatrix")
set_text(target, "CreateHexFile", "1")
set_text(target, "InvalidFlash", "0")
set_text(target, "ScatterFile", r".\output\PressureMatrix.sct")
set_text(target, "RegisterFile", r"$$Device:GD32E230K8$Device\Include\gd32e230.h")
set_text(target, "SFDFile", r"$$Device:GD32E230K8$SVD\GD32E230.svd")

flash_driver = target.find(".//FlashDriverDll")
if flash_driver is not None and flash_driver.text:
    flash_driver.text = flash_driver.text.replace("GD32E230C8", "GD32E230K8")

cads = target.find(".//Cads")
if cads is None:
    raise ValueError("C compiler options missing")
set_text(cads, "Define", "GD32E230")
set_text(
    cads,
    "IncludePath",
    r"..\App;..\Vendor\CMSIS;..\Vendor\CMSIS\GD\GD32E23x\Include;..\Vendor\GD32E23x_standard_peripheral\Include",
)

groups = target.find("Groups")
if groups is None:
    raise ValueError("Keil groups missing")
for child in list(groups):
    groups.remove(child)

add_group(
    groups,
    "Application",
    [(r"..\App\main.c", 1), (r"..\App\matrix.c", 1),
     (r"..\App\serial.c", 1), (r"..\App\systick.c", 1),
     (r"..\App\interrupts.c", 1), (r"..\App\matrix.h", 5),
     (r"..\App\serial.h", 5), (r"..\App\systick.h", 5),
     (r"..\App\gd32e23x_libopt.h", 5)],
)
add_group(
    groups,
    "CMSIS",
    [(r"..\Vendor\CMSIS\GD\GD32E23x\Source\system_gd32e23x.c", 1)],
)
add_group(
    groups,
    "GD32 peripherals",
    [(rf"..\Vendor\GD32E23x_standard_peripheral\Source\gd32e23x_{name}.c", 1)
     for name in ("adc", "gpio", "misc", "rcu", "usart")],
)
add_group(
    groups,
    "Startup",
    [(r"..\Vendor\CMSIS\GD\GD32E23x\Source\ARM\startup_gd32e23x.s", 2)],
)
add_group(groups, "Notes", [(r"..\README.md", 5)])

rte = root.find("RTE")
if rte is not None:
    root.remove(rte)

ET.indent(tree, space="  ")
tree.write(PROJECT, encoding="utf-8", xml_declaration=True)
print(PROJECT)
