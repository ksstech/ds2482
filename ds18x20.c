/*
 * Copyright 2018-21 Andre M. Maree/KSS Technologies (Pty) Ltd.
 */

#include	"hal_variables.h"
#include	"onewire_platform.h"
#include	"endpoints.h"
#include	"printfx.h"
#include	"syslog.h"
#include	"systiming.h"					// timing debugging
#include	"x_errors_events.h"

#include	<string.h>
#include	<stdint.h>

#define	debugFLAG					0xF001

#define	debugCONFIG					(debugFLAG & 0x0001)
#define	debugREAD					(debugFLAG & 0x0002)
#define	debugCONVERT				(debugFLAG & 0x0004)
#define	debugPOWER					(debugFLAG & 0x0008)

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ##################################### Developer notes ###########################################

/* DS18x20 is a 1-wire type device and thus BUS oriented:
 * 	multiple devices sharing a single bus.
 * 	each device can be individually R/W addressed
 * 	some operations eg temp sample/convert
 * 		happens reasonably slowly (up to 750mS)
 * 		can be triggered to execute in parallel for all "equivalent" devices on a bus
 *	To optimise operation, this driver is based on the following decisions/constraints:
 *		Tsns is specified at device type (psEWP level) for ALL /ow/ds18x20 devices
 *		always trigger a sample+convert operation for ALL devices on a bus at same time.
 *		maintains Tsns at a value equal to lowest Tsns specified for any one ds18x20 device
 *		maintain a minimum Tsns of 1000mSec to be bigger than the ~750mS standard.
 * 	Test parasitic power
 * 	Test & benchmark overdrive speed
 * 	Implement and test ALARM scan and over/under alarm status scan
 */

// ################################ Forward function declaration ###################################


// ######################################### Constants #############################################


// ###################################### Local variables ##########################################

ds18x20_t *	psaDS18X20	= NULL ;
uint8_t		Fam10Count, Fam28Count, Fam10_28Count ;

// #################################### Local ONLY functions #######################################

/**
 * @brief	Read power status bit, AI1 operation, select & release bus
 * @param	psDS18X20
 * @return	Power status
 */
int	ds18x20CheckPower(ds18x20_t * psDS18X20) {
	if (OWResetCommand(&psDS18X20->sOW, DS18X20_READ_PSU, 1) == 0) return 0 ;
	psDS18X20->Pwr = OWReadBit(&psDS18X20->sOW) ;					// return status 0=parasitic 1=external
	IF_PRINT(debugPOWER, "PSU=%s\n", psDS18X20->Pwr ? "Ext" : "Para") ;
	return psDS18X20->Pwr ;
}

// ###################################### scratchpad support #######################################

/**
 * @brief
 * @param	psDS18X20
 * @param	Len
 * @return
 * @note	Timing is as follows
 *	OWReset		196/1348uS
 *	OWCommand	1447/7740uS
 *	OWBlock		163/860 per byte, 326/1720 for temperature, 815/4300 for all.
 *	Total Time	1969/10808 for temperature
 */
int	ds18x20ReadSP(ds18x20_t * psDS18X20, int32_t Len) {
	if (OWResetCommand(&psDS18X20->sOW, DS18X20_READ_SP, 0) == 0) return 0 ;
	memset(psDS18X20->RegX, 0xFF, Len) ;				// 0xFF to read
	OWBlock(&psDS18X20->sOW, psDS18X20->RegX, Len) ;
	if (Len == SO_MEM(ds18x20_t, RegX)) OWCheckCRC(psDS18X20->RegX, SO_MEM(ds18x20_t, RegX)) ;
	else OWReset(&psDS18X20->sOW) ;						// terminate read
	return 1 ;
}

int	ds18x20WriteSP(ds18x20_t * psDS18X20) {
	if (OWResetCommand(&psDS18X20->sOW, DS18X20_WRITE_SP, 0) == 0) return 0 ;
	OWBlock(&psDS18X20->sOW, (uint8_t *) &psDS18X20->Thi, psDS18X20->sOW.ROM.Family == OWFAMILY_28 ? 3 : 2) ;	// Thi, Tlo [+Conf]
	return 1 ;
}

int	ds18x20WriteEE(ds18x20_t * psDS18X20) {
	if (OWResetCommand(&psDS18X20->sOW, DS18X20_COPY_SP, 0) == 0) return 0 ;
	vTaskDelay(pdMS_TO_TICKS(ds18x20DELAY_SP_COPY)) ;
	OWLevel(&psDS18X20->sOW, owPOWER_STANDARD) ;
	return 1 ;
}

// ################################ Basic temperature support ######################################

int	ds18x20TempRead(ds18x20_t * psDS18X20) { return ds18x20ReadSP(psDS18X20, 2) ; }

// ###################################### IRMACOS support ##########################################

int	ds18x20Initialize(ds18x20_t * psDS18X20) {
	if (ds18x20ReadSP(psDS18X20, SO_MEM(ds18x20_t, RegX))) return 0 ;
	psDS18X20->Res	= (psDS18X20->sOW.ROM.Family == OWFAMILY_28)
					? (psDS18X20->fam28.Conf >> 5)
					: owFAM28_RES9B ;
	psDS18X20->Pwr = ds18x20CheckPower(psDS18X20) ;
	ds18x20ConvertTemperature(psDS18X20) ;
#if		(debugCONFIG)
	OWP_PrintDS18_CB(makeMASKFLAG(1,1,0,0,0,0,0,0,0,0,0,0,psDS18X20->Idx), psDS18X20) ;
#endif
	return 1 ;
}

/**
 * @brief	reset device to default via SP, not written to EE
 * @param	psDS18X20
 * @return
 */
int	ds18x20ResetConfig(ds18x20_t * psDS18X20) {
	psDS18X20->Thi	= 75 ;
	psDS18X20->Tlo	= 70 ;
	if (psDS18X20->sOW.ROM.Family == OWFAMILY_28) psDS18X20->fam28.Conf = 0x7F ;	// 12 bit resolution
	ds18x20WriteSP(psDS18X20) ;
	return ds18x20Initialize(psDS18X20) ;
}

int	ds18x20ConvertTemperature(ds18x20_t * psDS18X20) {
	const uint8_t	u8Mask[4] = { 0xF8, 0xFC, 0xFE, 0xFF } ;
	uint16_t u16Adj = (psDS18X20->Tmsb << 8) | (psDS18X20->Tlsb & u8Mask[psDS18X20->Res]) ;
	psDS18X20->sEWx.var.val.x32.f32 = (float) u16Adj / 16.0 ;
#if		(debugCONVERT)
	OWP_PrintDS18_CB(makeMASKFLAG(1,1,0,0,0,0,0,0,0,0,0,0,psDS18X20->Idx), psDS18X20) ;
#endif
	return 1 ;
}

// ################################ Rules configuration support ####################################

int	ds18x20SetResolution(ds18x20_t * psDS18X20, int Res) {
	if (psDS18X20->sOW.ROM.Family == OWFAMILY_28 && INRANGE(9, Res, 12, int)) {
		uint8_t u8Res = ((Res - 9) << 5) | 0x1F ;
		if (psDS18X20->fam28.Conf != u8Res) {
			IF_PRINT(debugCONFIG, "SP Res:0x%02X -> 0x%02X (%d -> %d)\n",
					psDS18X20->fam28.Conf, u8Res, psDS18X20->Res + 9, Res) ;
			psDS18X20->fam28.Conf = Res ;
			psDS18X20->Res = Res - 9 ;
			ds18x20WriteSP(psDS18X20) ;
			return 1 ;
		}
		// Not written, config already the same
		return 0 ;
	}
	SET_ERRINFO("Invalid Family/Resolution") ;
	return erSCRIPT_INV_VALUE ;
}

int	ds18x20SetAlarms(ds18x20_t * psDS18X20, int Lo, int Hi) {
	if (INRANGE(-128, Lo, 127, int) && INRANGE(-128, Hi, 127, int)) {
		if (psDS18X20->Tlo != Lo || psDS18X20->Thi != Hi) {
			IF_PRINT(debugCONFIG, "SP Tlo:%d -> %d  Thi:%d -> %d\n", psDS18X20->Tlo, Lo, psDS18X20->Thi, Hi) ;
			psDS18X20->Tlo = Lo ;
			psDS18X20->Thi = Hi ;
			ds18x20WriteSP(psDS18X20) ;
			return 1 ;									// new config written
		}
		return 0 ;										// Not written, config already the same
	}
	SET_ERRINFO("Invalid Lo/Hi alarm limits") ;
	return erSCRIPT_INV_VALUE ;
}

int	ds18x20ConfigMode (struct rule_t * psRule) {
	if (psaDS18X20 == NULL) {
		SET_ERRINFO("No DS18x20 enumerated") ;
		return erSCRIPT_INV_OPERATION ;
	}
	// support syntax mode /ow/ds18x20 idx lo hi res [1=persist]
	uint8_t	AI = psRule->ActIdx ;
	epw_t * psEW = &table_work[psRule->actPar0[AI]] ;
	px_t	px ;
	px.pu32 = (uint32_t *) &psRule->para.u32[AI][0] ;
	int	Xcur = *px.pu32++ ;
	int Xmax = psEW->var.def.cv.vc ;
	if (Xcur == 255) {									// non-specific total count ?
		Xcur = Xmax ;									// yes, set to actual count.
	} else if (Xcur > Xmax) {
		SET_ERRINFO("Invalid EP Index") ;
		return erSCRIPT_INV_INDEX ;
	}
	if (Xcur == Xmax) Xcur = 0 ; 						// range 0 -> Xmax
	else Xmax = Xcur ;									// single Xcur
	uint32_t lo	= *px.pu32++ ;
	uint32_t hi	= *px.pu32++ ;
	uint32_t res = *px.pu32++ ;
	uint32_t wr	= *px.pu32 ;
	IF_PRINT(debugCONFIG, "DS18X20 Mode Xcur=%d lo=%d hi=%d res=%d wr=%d\n", Xcur, lo, hi, res, wr) ;
	int iRV = 0 ;
	if (wr == 0 || wr == 1) {							// if parameter omitted, do not persist
		do {
			ds18x20_t * psDS18X20 = &psaDS18X20[Xcur] ;
			if (OWP_BusSelect(&psDS18X20->sOW) == 1) {
				// Do resolution 1st since small range (9-12) a good test for valid parameter
				iRV = ds18x20SetResolution(psDS18X20, res) ;
				if (iRV >= erSUCCESS) {
					iRV = ds18x20SetAlarms(psDS18X20, lo, hi) ;
					if (iRV >= erSUCCESS && wr == 1) iRV = ds18x20WriteSP(psDS18X20) ;
				}
				OWP_BusRelease(&psDS18X20->sOW) ;
			}
			if (iRV < erSUCCESS) break ;
		} while (++Xcur < Xmax) ;
	} else {
		SET_ERRINFO("Invalid persist flag, not 0/1") ;
		iRV = erSCRIPT_INV_MODE ;
	}
	return iRV ;
}

// ################################## Platform enumeration support #################################

// ######################################### Reporting #############################################

void	ds18x20ReportAll(void) {
	for (int i = 0; i < Fam10_28Count; ++i)
		OWP_PrintDS18_CB(makeMASKFLAG(0,1,0,0,0,1,1,1,1,1,1,1,i), &psaDS18X20[i]) ;
}

