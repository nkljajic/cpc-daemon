/***************************************************************************//**
 * @file
 * @brief Co-Processor Communication Protocol (CPC) - Driver kill
 *******************************************************************************
 * # License
 * <b>Copyright 2022 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

#ifndef DRIVER_KILL_H
#define DRIVER_KILL_H

int driver_kill_init(void);

void driver_kill_signal(void);

int driver_kill_join(void);

int driver_kill_signal_and_join(void);

#endif //DRIVER_KILL_H
