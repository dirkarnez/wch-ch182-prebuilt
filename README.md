# CH182 PHY Device Tree Bindings Documentation

This document describes the device tree bindings for the CH182 family of Ethernet PHYs developed by **WCH**.

## Supported PHY Models

* CH182D
* CH182F2
* CH182F7
* CH182F8
* CH182H1
* CH182H2
* CH182H3
* CH182H6
* CH182H7
* CH182H8

## LED Mode Configuration

The CH182 PHYs support two LED configuration modes: **traditional** and **custom**.

> ⚠️ Only one of these modes should be used at a time. Do not configure both traditional and custom LED modes simultaneously in the device tree.

### 1. Traditional LED Mode

When using the traditional LED configuration, the mode is set using Page 7 register `0x13`, bits `[5:4]`. This configuration applies to all CH182 series PHYs.

To enable this mode via device tree, use:

```dts
wch,traditional-led = <1>; // Mode value (0-3), corresponds to bits [5:4] in register 0x13
```

Refer to the CH182DS2 datasheet for valid traditional LED mode values and their meanings.

### 2. Custom LED Mode

In custom LED configuration mode, the modes are set using Page 7 register `0x11`:

* `bit[7:4]`: LED0
* `bit[3:0]`: LED1

Use the following properties in the device tree to configure:

```dts
wch,led0-mode = <1>; // Set LED0 mode: binary 0b0001
wch,led1-mode = <7>; // Set LED1 mode: binary 0b0111
```

> The value must be an integer in the range `<0>` to `<7>`, representing 4-bit binary mode settings (e.g., `<7>` means `0b0111`).

Refer to the [LED configuration table](https://www.cnblogs.com/llidd/p/18748573) or the **CH182DS2 datasheet** for valid mode values and descriptions.

## Clock Configuration

### Clocks and Clock Names

The CH182 PHYs support RMII reference clock input to determine the XI input clock. Use the common clock bindings for clocks and clock-names:

```dts
clocks = <&clks IMX6UL_CLK_ENET_REF>;
clock-names = "rmii-ref";
```

## Example Device Tree Snippet

```dts
&fec1 {
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_enet1>;
    phy-mode = "rmii";
    phy-handle = <&ethphy1>;
    status = "okay";

    mdio: mdio {
        #address-cells = <1>;
        #size-cells = <0>;

        ethphy1: ethernet-phy@1 {
            compatible = "ethernet-phy-ieee802.3-c22";
            reg = <1>; // MDIO bus address
            wch,traditional-led = <1>; // Use either this (0-3)
            // wch,led0-mode = <1>;   // or these two (0-15, 4-bit binary), use either this (0-7)
            // wch,led1-mode = <7>;  // Use either this (0-7)
            clocks = <&clks IMX6UL_CLK_ENET_REF>;
            clock-names = "rmii-ref";
        };
    };
};
```

---

Please refer to the official CH182 datasheet for further register configuration details and full mode definitions.
Any questions, you can send feedback to mail: [tech@wch.cn].
