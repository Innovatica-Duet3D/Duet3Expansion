/* Auto-generated config file peripheral_clk_config.h */
#ifndef PERIPHERAL_CLK_CONFIG_H
#define PERIPHERAL_CLK_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>

/**
 * \def CONF_CPU_FREQUENCY
 * \brief CPU's Clock frequency
 */
#ifndef CONF_CPU_FREQUENCY
#define CONF_CPU_FREQUENCY 120000000
#endif

#ifndef CONF_GCLK_SERCOM2_CORE_SRC
#define CONF_GCLK_SERCOM2_CORE_SRC GCLK_PCHCTRL_GEN_GCLK1_Val
#endif

// <i> Select the slow clock source.
#ifndef CONF_GCLK_SERCOM2_SLOW_SRC
#define CONF_GCLK_SERCOM2_SLOW_SRC GCLK_PCHCTRL_GEN_GCLK1_Val
#endif

/**
 * \def CONF_GCLK_SERCOM2_CORE_FREQUENCY
 * \brief SERCOM2's Core Clock frequency
 */
#ifndef CONF_GCLK_SERCOM2_CORE_FREQUENCY
#define CONF_GCLK_SERCOM2_CORE_FREQUENCY 60000000
#endif

/**
 * \def CONF_GCLK_SERCOM2_SLOW_FREQUENCY
 * \brief SERCOM2's Slow Clock frequency
 */
#ifndef CONF_GCLK_SERCOM2_SLOW_FREQUENCY
#define CONF_GCLK_SERCOM2_SLOW_FREQUENCY 12000000
#endif


// CAN1 Clock Source
#ifndef CONF_GCLK_CAN1_SRC
#define CONF_GCLK_CAN1_SRC GCLK_PCHCTRL_GEN_GCLK2_Val
#endif

/**
 * \def CONF_GCLK_CAN1_FREQUENCY
 * \brief CAN1's Clock frequency
 */
#ifndef CONF_GCLK_CAN1_FREQUENCY
#define CONF_GCLK_CAN1_FREQUENCY 48000000
#endif


// <<< end of configuration section >>>

#endif // PERIPHERAL_CLK_CONFIG_H
