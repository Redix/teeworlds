#ifndef GAME_GHOST_H
#define GAME_GHOST_H

#include <generated/protocol.h>

enum
{
	GHOSTDATA_TYPE_SKIN_06 = 0,
	GHOSTDATA_TYPE_CHARACTER_NO_TICK,
	GHOSTDATA_TYPE_CHARACTER,
	GHOSTDATA_TYPE_START_TICK,
	GHOSTDATA_TYPE_SKIN,
	GHOSTDATA_TYPE_TEAM
};

struct CGhostSkin_06
{
	int m_aSkin[6];
	int m_UseCustomColor;
	int m_ColorBody;
	int m_ColorFeet;
};

struct CGhostSkin
{
	int m_aaSkinPartNames[6][6];
	int m_aUseCustomColors[6];
	int m_aSkinPartColors[6];
};

struct CGhostCharacter_NoTick
{
	int m_X;
	int m_Y;
	int m_VelX;
	int m_VelY;
	int m_Angle;
	int m_Direction;
	int m_Weapon;
	int m_HookState;
	int m_HookX;
	int m_HookY;
	int m_AttackTick;
};

struct CGhostCharacter : public CGhostCharacter_NoTick
{
	int m_Tick;
};

class CGhostTools
{
public:
	static void GetGhostSkin(CGhostSkin *pSkin, const char aaSkinPartNames[NUM_SKINPARTS][24], const int *pUseCustomColors, const int *pSkinPartColors)
	{
		for(int p = 0; p < NUM_SKINPARTS; p++)
		{
			StrToInts(pSkin->m_aaSkinPartNames[p], 6, aaSkinPartNames[p]);
			pSkin->m_aUseCustomColors[p] = pUseCustomColors[p];
			pSkin->m_aSkinPartColors[p] = pSkinPartColors[p];
		}
	}

	static void GetGhostCharacter(CGhostCharacter *pGhostChar, const CNetObj_Character *pChar)
	{
		pGhostChar->m_X = pChar->m_X;
		pGhostChar->m_Y = pChar->m_Y;
		pGhostChar->m_VelX = pChar->m_VelX;
		pGhostChar->m_VelY = 0;
		pGhostChar->m_Angle = pChar->m_Angle;
		pGhostChar->m_Direction = pChar->m_Direction;
		pGhostChar->m_Weapon = pChar->m_Weapon;
		pGhostChar->m_HookState = pChar->m_HookState;
		pGhostChar->m_HookX = pChar->m_HookX;
		pGhostChar->m_HookY = pChar->m_HookY;
		pGhostChar->m_AttackTick = pChar->m_AttackTick;
		pGhostChar->m_Tick = pChar->m_Tick;
	}

	static void GetNetObjCharacter(CNetObj_Character *pChar, const CGhostCharacter *pGhostChar)
	{
		mem_zero(pChar, sizeof(CNetObj_Character));
		pChar->m_X = pGhostChar->m_X;
		pChar->m_Y = pGhostChar->m_Y;
		pChar->m_VelX = pGhostChar->m_VelX;
		pChar->m_VelY = 0;
		pChar->m_Angle = pGhostChar->m_Angle;
		pChar->m_Direction = pGhostChar->m_Direction;
		pChar->m_Weapon = pGhostChar->m_Weapon == WEAPON_GRENADE ? WEAPON_GRENADE : WEAPON_GUN;
		pChar->m_HookState = pGhostChar->m_HookState;
		pChar->m_HookX = pGhostChar->m_HookX;
		pChar->m_HookY = pGhostChar->m_HookY;
		pChar->m_AttackTick = pGhostChar->m_AttackTick;
		pChar->m_HookedPlayer = -1;
		pChar->m_Tick = pGhostChar->m_Tick;
	}
};

#endif

