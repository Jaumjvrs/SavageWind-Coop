"""Build the co-op RomFS overlay (SAVAGEWIND_ROMFS_OVERLAY) from the player's own game files.

Adds Dm_CoopLink: a cutscene-style puppet (profile DemoNPC, no physics, AI DemoDummy) that uses Link's model and the
player's animation sequences, for showing the other player. Moved by the runtime writing its matrix (runtime/coop.c).
Test variants, one change at a time from the working Dm_PlayerStole2 (paraglider cloth puppet):
  Dm_CoopTestA  = Dm_PlayerStole2 renamed
  Dm_CoopTestB  = A with Link's model list (no player animations)
  Dm_CoopLink   = B with the player's AS list and AS files

Usage: python build_overlay.py [--play] [--game <extracted
omfs>] [--out <overlay dir>]
  --play  only Dm_CoopLinkE, the avatar the co-op build uses (what a player needs; the default builds every test variant)
Default output: local/overlay/ next to this script (never shipped: it is made of game data; each player builds it from their own
files). Needs Python with oead (pip install oead).
ResourceSizeTable entries for the new packs are added by the runtime (runtime/services/fs.c rstb_overlay).
"""
import argparse
import os
import re
import sys
import zlib

import oead

# (the portable Python in py\ has a ._pth file: the script's own folder is not on the path there, 2026-11-17)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from astree_tables import write_tables

GAME = r'C:\Savage Wind\extracted\romfs'
# (2026-11-20) Each avatar / puppet actor's own heap (ActorInfo instSize) x HEAP_SCALE. With 3 players the second
# extra avatar's animation setup got a null block from its heap and host + guests crashed (a three-player rejoin test);
# x2 passed. Cost ~2 MB per drawn avatar. COOP_HEAP_SCALE=1 restores the old sizes.
# (2026-11-21) x2 still crashed the same way 3 times in the 11-20 evening play (avatar/armor/horse puppet created right
# after a shrine load: AS node allocation from the actor's heap returned null): x3, and the horse puppets get it too.
HEAP_SCALE = float(os.environ.get('COOP_HEAP_SCALE', '3'))
OVERLAY_VERSION = '2026-11-21-heap3'  # written to coop_overlay_version.txt; play_coop.ps1 expects the same text
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'local', 'overlay')


# Single-clip AS for the avatar (name, clip in Player_Animation); names must match runtime/coop.c coop_anim_as[].
COOP_CLIPS = [
    ('CoopWait', 'Nml_Wait'), ('CoopWalk', 'Nml_Move_Walk'), ('CoopRun', 'Nml_Move_Run'),
    ('CoopDash', 'Nml_Move_Dash'), ('CoopJump', 'Nml_Wait_Jump'), ('CoopFall', 'Fall'),
    ('CoopGlide', 'Float_Float_F'), ('CoopSwimWait', 'Swim_Wait'), ('CoopSwim', 'Swim_Move_Slow_F'),
    ('CoopSwimFast', 'Swim_Move_Dash'), ('CoopClimbWait', 'Climb_Wall_Wait_FootL'), ('CoopClimb', 'Climb_Wall_Move_U'),
    ('CoopCut1', 'Sword_Attack_S1'), ('CoopCut2', 'Sword_Attack_S2'), ('CoopCut3', 'Sword_Attack_S3'),
    ('CoopCutFinish', 'Sword_Attack_SF'), ('CoopCutDash', 'Sword_Attack_Dash'), ('CoopCutSpin', 'Sword_Attack_Charge'),
    # (protocol 14) more states, and the two-handed sword / spear swings (runtime/coop.c avatar_as)
    ('CoopSquatWait', 'Squat_Wait_FootL'), ('CoopSquatMove', 'Squat_Move_Walk'), ('CoopGuard', 'Shield_Guard_Wait'),
    ('CoopBow', 'Bow_Attack_Charge'), ('CoopHorseWait', 'Horse_Wait'), ('CoopHorseRun', 'Horse_Move_Gear_2'),
    ('CoopClimbDown', 'Climb_Wall_Move_D'), ('CoopClimbLeft', 'Climb_Wall_Move_L'),
    ('CoopClimbRight', 'Climb_Wall_Move_R'),
    ('CoopLsword1', 'Lsword_Attack_S1'), ('CoopLswordFinish', 'Lsword_Attack_SF'),
    ('CoopSpear1', 'Spear_Attack_S1'), ('CoopSpearFinish', 'Spear_Attack_SF'),
    # (protocol 19) expanded actions: parry, dodges, landings, damage, bow release, death, charge attacks
    ('CoopGuardJust', 'Shield_Guard_Just'), ('CoopBackJump', 'A_Pose_Lsword_BackJump'),
    ('CoopSideHop', 'A_Pose_Lsword_Step'), ('CoopLand', 'A_Pose_Sword_Land'),
    ('CoopDamage', 'Nml_Damage_L_B'), ('CoopBowShoot', 'Bow_Attack_Shoot'),
    ('CoopDie', 'Nml_Die'), ('CoopLswordSpin', 'Lsword_Attack_Charge'),
    ('CoopSpearRush', 'Spear_Attack_Charge_Rush'),
    # (protocol 20) weapon throw aim & throw, object carry & throw
    ('CoopThrowAim', 'Sword_Throw_Charge_Wait'), ('CoopThrow', 'Sword_Throw_Throw'),
    ('CoopLswordThrowAim', 'Lsword_Throw_Charge_Wait'), ('CoopLswordThrow', 'Lsword_Throw_Throw'),
    ('CoopSpearThrowAim', 'Spear_Throw_Charge_Wait'), ('CoopSpearThrow', 'Spear_Throw_Throw'),
    ('CoopCarry', 'Lift_Up_Barrel'), ('CoopCarryThrow', 'Lift_Throw_Barrel'),
    # (protocol 23) movement family: shield surfing, ladders, slope slide, tumbling, wall jump, pushing, tired, brake
    ('CoopSurf', 'Shield_Board_Go2'), ('CoopSurfAir', 'Shield_Board_Fall_Loop'), ('CoopSurfJump', 'Shield_Board_Jump'),
    ('CoopSurfLand', 'Shield_Board_Land'), ('CoopSurfOn', 'Shield_Board_ON'), ('CoopSurfOff', 'Shield_Board_OFF'),
    ('CoopSurfStagger', 'Shield_Board_Stagger_Slope_0'),
    ('CoopLadderWait', 'Ladder_Wait_FootL'), ('CoopLadderUp', 'Ladder_Up_FootR'), ('CoopLadderDown', 'Ladder_Down'),
    ('CoopSlide', 'Slide_F_50'), ('CoopRolldown', 'Rolldown'), ('CoopClimbDash', 'Wall_Dash_FootL'),
    ('CoopPush', 'Push_Roll_Walk_ShoulderL'), ('CoopTired', 'Nml_Tired_Wait'), ('CoopBrake', 'Nml_Move_Run_Brake_FootL'),
    # (protocol 24) items and the Sheikah Slate (full body), then runes/items held while moving (upper body layer)
    ('CoopGet', 'Nml_Get'), ('CoopChestKick', 'Demo_Get_Kick_TreasureBox'), ('CoopChestOpen', 'Demo_Get_Unlock_Treasurebox'),
    ('CoopSlateSet', 'Demo_Common_SheikPad_Set_St'), ('CoopSlateLook', 'Demo_Common_SheikPad_LookPad'),
    ('CoopSlateTouch', 'Demo_Common_SheikPad_Touch'), ('CoopSlateDownload', 'Demo_Common_SheikPad_DownLoad_Ready_St'),
    ('CoopSlateWaist', 'Demo_Common_SheikPad_LookWaist'),
    ('CoopCookGood', 'Demo_Get_Cooking_Good_Loop'), ('CoopCookBad', 'Demo_Get_Cooking_Bad_Loop'),
    ('CoopLookAround', 'Nml_Lookaround'), ('CoopDive', 'Swim_Dive_St'),
    ('CoopRuneAim', 'Item_IceMaker_Ready'), ('CoopBombReady', 'Item_Bomb_Ready'), ('CoopMagne', 'MagneCatch_Wait'),
    ('CoopScope', 'Item_Scope_Ready'), ('CoopEquipOn', 'Equip_Sword_On_Natural'),
    ('CoopEquipOff', 'Equip_Sword_Off_Natural'), ('CoopWhistle', 'Horse_Call'),
    # (protocol 26) talking, doors, mounting, sand-seal surfing, warp, frozen, knocked down, swim/climb extras
    ('CoopTalk', 'Nml_Talk'), ('CoopTalking', 'Nml_Talk_Talk_L'), ('CoopTalkShow', 'Nml_Talk_Weapon_Show_Sword'),
    ('CoopTalkSlate', 'Nml_Talk_SheikPad_Show_On'), ('CoopHorseSoothe', 'Horse_Move_Soothe'),
    ('CoopHorseRodeo', 'Horse_Rodeo'), ('CoopWakeboard', 'WakeBoard_Normal'),
    ('CoopWakeboardAir', 'WakeBoard_Air'), ('CoopFreeze', 'Nml_Damage_Freeze'),
    ('CoopUnsteady', 'Nml_Wait_Unsteady'), ('CoopWaterfall', 'Swim_Waterfall_Up'),
    ('CoopClimbSlip', 'Climb_Slip_FootR'), ('CoopTalkHello', 'Nml_Talk_Hello'), ('CoopTalkYes', 'Nml_Talk_Yes'),
    ('CoopTalkSurprise', 'Nml_Talk_Surprise'), ('CoopTalkEmbarrass', 'Nml_Talk_Embarrass'),
    ('CoopTalkHandover', 'Nml_Talk_HoldOut'), ('CoopDoorPush', 'Door_Push_R'), ('CoopDoorSlide', 'Door_Slide_R'),
    ('CoopMount', 'Horse_Rideon_WaitHorse_Ed_R90'), ('CoopDismount', 'Horse_Dismount_R90'),
    ('CoopHorseJump', 'Horse_Jump_Gear_3'), ('CoopWarp', 'Demo_Warp_Nml'), ('CoopFreezeBreak', 'Freeze_Break'),
    ('CoopLandDamage', 'Nml_Damage_Land'), ('CoopStandup', 'Nml_Damage_L_F_Standup'),
    ('CoopGuardBreak', 'Shield_Guard_Break'), ('CoopGuardHit', 'Shield_Guard_Hit'), ('CoopSwimDie', 'Swim_Die'),
    ('CoopSwimJump', 'Swim_Jump_St'), ('CoopSwimAttack', 'Swim_Attack'),
    ('CoopClimbJump', 'Climb_Wall_Jump_U_FootR'), ('CoopClimbEdge', 'Climb_Wall_Ed_Edge_L'),
    ('CoopParaOn', 'Equip_Float_On'), ('CoopStagger', 'Nml_Stagger_U'),
    # (protocol 27) idles: random fidgets, hot / cold, lock-on stance (by weapon), hurt; and the upper-body AS of
    # standing with a weapon or bow in the hand (runtime/coop/avatar.inc armed_idle_as)
    ('CoopIdleSleepy', 'Random_Nml_Wait_Nemui'), ('CoopIdleRelax', 'Random_Nml_Wait_RelaxUpper'),
    ('CoopIdleFlex', 'Random_Nml_Wait_Kinnikujiman'), ('CoopIdleWipe', 'Random_Squat_Wait_WipeHand_FootL'),
    ('CoopColdFidget', 'Nml_Cold_Wait_Random'), ('CoopHotFidget', 'Nml_Hot_Wait_Random'),
    ('CoopColdWait', 'Nml_Cold_Wait'), ('CoopHotWait', 'Nml_Hot_Wait'),
    ('CoopLockonWait', 'Nml_LockonBattle_Wait'), ('CoopHurtWait', 'Nml_Damage_Wait'),
    ('CoopLockonSword', 'Sword_Lockon_Wait'), ('CoopLockonLsword', 'Lsword_Lockon_Wait'),
    ('CoopLockonSpear', 'Spear_Lockon_Wait'),
    ('CoopArmSword', 'A_Pose_Sword_Wait'), ('CoopArmLsword', 'A_Pose_Lsword_Wait'),
    ('CoopArmSpear', 'A_Pose_Spear_Wait'), ('CoopArmBow', 'A_Pose_Bow_Wait'),
    # (protocol 28) jump / horse attacks, horse extras, putting down, climb extras, selfie, turns; weapon variants
    ('CoopCutJumpLoop', 'Sword_Attack_Jump_Loop'), ('CoopCutFall', 'Sword_Attack_Fall'),
    ('CoopCutHorseCharge', 'Sword_Charge_Horse_Wait'), ('CoopSelfie', 'SelfCameraPose_RaiseHandL'),
    ('CoopDemoLookaround', 'Nml_Lookaround'), ('CoopCutJumpSt', 'Sword_Attack_Jump_F_St'),
    ('CoopCutJumpLand', 'Sword_Attack_Jump_F_Ed'), ('CoopCutFallLand', 'Sword_Attack_Fall_Ed'),
    ('CoopCutHorse', 'Sword_Attack_Horse_R'), ('CoopHorseCrash', 'Horse_Crash'),
    ('CoopHorseSlip', 'Horse_Move_Slip'), ('CoopHorseCourbette', 'Horse_Courbette_Gear_3'),
    ('CoopHorseDead', 'Nml_Die_Horse_R'), ('CoopHorseSpur', 'Horse_Move_Gear_3_Go'),
    ('CoopGrabPut', 'Lift_Put_Rock'), ('CoopParaOff', 'Equip_Float_Off'),
    ('CoopLiftPutPouch', 'Demo_Lift_Put_Pouch_L'), ('CoopClimbTreeTop', 'Climb_Tree_Top_Sharp_FootL'),
    ('CoopClimbTreeEd', 'Climb_Tree_Ed_Sharp'), ('CoopClimbTreeTurn', 'Climb_Tree_Turn_Sharp_R0_FootLtoR'),
    ('CoopGroundDownWall', 'Climb_Wall_DownToWall'), ('CoopLadderJump', 'Ladder_Jump_FootL'),
    ('CoopLadderToClimb', 'Climb_Wall_Jump_L_FootL'), ('CoopWallOff', 'Climb_Wall_Off_Tired_FootL'),
    ('CoopWallOffSquat', 'Climb_WallOff_ToSquat_FootL'), ('CoopMasterDead', 'MasterSwordDead'),
    ('CoopTurn', 'Nml_Move_Run_Brake_FootL'), ('CoopTurnLockon', 'Nml_Lockon_Turn_R'),
    ('CoopDemoTurn', 'Demo_Turn_Wait90R'), ('CoopEquipNg', 'Equip_Empty_On_Natural'),
    ('CoopBowReload', 'Bow_Attack_Load'), ('CoopThrowCatch', 'Sword_Throw_Catch'),
    ('CoopItemLOn', 'Equip_Item_L_On'), ('CoopItemLOff', 'Equip_Item_L_Off'),
    ('CoopCutJumpLoopLsword', 'Lsword_Attack_Jump_Loop'), ('CoopCutJumpLoopSpear', 'Spear_Attack_Jump_D30'),
    ('CoopCutFallLsword', 'Lsword_Attack_Jump_Loop'), ('CoopCutFallSpear', 'Spear_Attack_Fall'),
    ('CoopCutHorseChargeLsword', 'Lsword_Charge_Horse_Wait'),
    ('CoopCutHorseChargeSpear', 'Spear_Charge_Horse_Wait'), ('CoopSelfieLsword', 'SelfCameraPose_Lsword'),
    ('CoopSelfieSpear', 'SelfCameraPose_Spear'), ('CoopCutJumpStLsword', 'Lsword_Attack_Jump'),
    ('CoopCutJumpStSpear', 'Spear_Attack_Jump_D60'), ('CoopCutJumpLandLsword', 'Lsword_Attack_Jump_Land'),
    ('CoopCutJumpLandSpear', 'Spear_Attack_Jump_F_Ed'), ('CoopCutFallLandLsword', 'Lsword_Attack_Jump_Land'),
    ('CoopCutFallLandSpear', 'Spear_Attack_Fall_Ed'), ('CoopCutHorseLsword', 'Lsword_Attack_Horse_R'),
    ('CoopCutHorseSpear', 'Spear_Attack_Horse_R30'), ('CoopLswordThrowCatch', 'Lsword_Throw_Catch'),
    # (protocol 81, runtime/coop/eat.inc) eating / drinking from the inventory: the pause-menu Link's UseItem cases
    # (PauseMenuPlayer_UseItem.bas: EatFood, EatCookingDish, EatCookingSkewer, EatBizarre, EatTooHard, Drink, UseFairy)
    ('CoopEatFood', 'Eat_OneHand_R'), ('CoopEatDish', 'Eat_Normal'), ('CoopEatSkewer', 'Eat_BBQ'),
    ('CoopEatBizarre', 'Eat_LookBad'), ('CoopEatHard', 'Eat_Hard'), ('CoopEatDrink', 'Eat_Drink'),
    ('CoopEatFairy', 'Eat_Fairy'),
]
# (protocol 81) The face of each eat case: the branch of PauseMenuPlayer_FaceUseItem.bas for that case (its root is a
# UseItemSelector; child i = case i), copied to the root of its own AS (face clip + face texture animation).
EAT_FACES = ['CoopXFaceEatFood', 'CoopXFaceEatDish', 'CoopXFaceEatSkewer', 'CoopXFaceEatBizarre', 'CoopXFaceEatHard',
             'CoopXFaceEatDrink', 'CoopXFaceEatFairy']


def eat_face_files():
    """{AS name: .bas bytes} for EAT_FACES, from the player's PauseMenuPlayer pack ({} when it is missing)."""
    path = os.path.join(GAME, 'Actor', 'Pack', 'PauseMenuPlayer.sbactorpack')
    if not os.path.exists(path):
        return {}
    pk = oead.Sarc(oead.yaz0.decompress(open(path, 'rb').read()))
    raw = bytes(pk.get_file('Actor/AS/PauseMenuPlayer_FaceUseItem.bas').data)
    out = {}
    for i, name in enumerate(EAT_FACES):
        pio = oead.aamp.ParameterIO.from_binary(raw)
        els = pio.lists['Elements'].lists
        root = els['Element0']
        if int(root.objects['Parameters'].params['TypeIndex'].v) != 87:  # (UseItemSelector)
            return {}
        k = int(root.objects['Children'].params[f'Child{i}'].v)
        els['Element0'] = oead.aamp.ParameterIO.from_binary(raw).lists['Elements'].lists[f'Element{k}']
        out[name] = pio.to_binary()
    return out


def setp(obj, key, value):
    """Set a parameter keeping its type (string ref, fixed-size string, bool...)."""
    old = obj.params[key].v
    if isinstance(old, (oead.FixedSafeString32, oead.FixedSafeString64)):
        value = type(old)(value)
    elif type(old) is int:  # (a plain Python int would become an F32)
        value = oead.S32(value)
    obj.params[key] = oead.aamp.Parameter(value)


def aamp(sarc, path):
    return oead.aamp.ParameterIO.from_binary(bytes(sarc.get_file(path).data))


def make_pack(stole, player, name, link_model, player_as, profile=None, model_user='CoopLink', bow_as=None):
    """An actor pack from the Dm_PlayerStole2 one (same SARC settings), with its own name.
    player_as: True = the player's AS list + the Coop* AS ('CoopLink'); 'coop' = only the Coop* AS ('CoopArmor', for
    the armor puppets that play the avatar's AS in step with it)."""
    w = oead.SarcWriter.from_sarc(stole)
    w.files.clear()
    link = aamp(stole, 'Actor/ActorLink/Dm_PlayerStole2.bxml')
    t = link.objects['LinkTarget']
    setp(t, 'GParamUser', name)
    if profile:
        setp(t, 'ProfileUser', profile)
    gparam = aamp(stole, 'Actor/GeneralParamList/Dm_PlayerStole2.bgparamlist')
    w.files['Actor/AIProgram/DemoDummy.baiprog'] = bytes(stole.get_file('Actor/AIProgram/DemoDummy.baiprog').data)
    if link_model:  # 'player': the player's model list; (folder, unit): the puppet's list with that model in it
        setp(t, 'ModelUser', model_user)  # (own name per actor: resources are cached by path)
        # (2026-11-14) Dm_CoopLinkF: the avatar with the player's own effect set (swim ripples, splashes, dust...;
        # runtime/coop/avatarfx.inc writes the other player's effect properties into it)
        setp(t, 'ElinkUser', 'Player_GameRom' if name in (AVATAR_FX_ACTOR, AVATAR_SND_ACTOR) else 'Dummy')
        # (2026-11-14) Dm_CoopLinkS: also the player's own sound set (SLink user GameROMPlayer): the voice / action
        # sounds the player's AS start play on the avatar (runtime/coop/avatarsnd.inc)
        if name == AVATAR_SND_ACTOR:
            setp(t, 'SlinkUser', 'GameROMPlayer')
        # (2026-11-14) weapon puppets keep their real weapon's sound set (sword swing sounds, avatarsnd.inc)
        if name in PUPPET_SLINK:
            setp(t, 'SlinkUser', PUPPET_SLINK[name])
        # (2026-11-16) melee weapon puppets keep their real weapon's effect set too (ElinkUser WeaponSpear /
        # WeaponLsword / Weapon_Sword_001...: SpearAttack, Blur_Default trails; runtime/coop/avatarfx.inc fires them)
        if name in PUPPET_ELINK:
            setp(t, 'ElinkUser', PUPPET_ELINK[name])
        setp(gparam.objects['Event'], 'VisibleOffActor1', '')
        if isinstance(link_model, (tuple, list)):  # one (folder, unit), or a list of them (one ModelData each)
            parts = [link_model] if isinstance(link_model, tuple) else link_model
            models = aamp(stole, 'Actor/ModelList/PlayerStole2.bmodellist')
            for i, (folder, unit) in enumerate(parts):
                md = aamp(stole, 'Actor/ModelList/PlayerStole2.bmodellist').lists['ModelData'].lists['ModelData_0']
                setp(md.objects['Base'], 'Folder', folder)
                setp(md.lists['Unit'].objects['Unit_0'], 'UnitName', unit)
                models.lists['ModelData'].lists[f'ModelData_{i}'] = md
            if player_as:
                # Multi-slot skeletal blending: copy slot definitions from Player_Link.bmodellist (slots 0..4)
                pmodels = aamp(player, 'Actor/ModelList/Player_Link.bmodellist')
                models.lists[oead.aamp.Name(4041283375)] = pmodels.lists[oead.aamp.Name(4041283375)]
        else:
            models = aamp(player, 'Actor/ModelList/Player_Link.bmodellist')
            setp(models.objects['Attention'], 'IsEnableAttention', False)
        w.files[f'Actor/ModelList/{model_user}.bmodellist'] = models.to_binary()
        if name in (AVATAR_FX_ACTOR, AVATAR_SND_ACTOR):
            # The SwordBlur callback reads the wielder's per-animation trail samples.
            setp(t, 'AnimationInfo', 'CoopLink')
            w.files['Actor/AnimationInfo/CoopLink.baniminfo'] = bytes(
                player.get_file('Actor/AnimationInfo/GameROMPlayer.baniminfo').data)
    else:
        w.files['Actor/ModelList/PlayerStole2.bmodellist'] = bytes(stole.get_file('Actor/ModelList/PlayerStole2.bmodellist').data)
    if player_as:
        # (its own name: the game caches resources by path, so a "Player" list would be the player's already-loaded one)
        slim = player_as == 'coop'
        as_user = 'CoopArmor' if slim else 'CoopLink'
        setp(t, 'ASUser', as_user)
        aslist = aamp(player, 'Actor/ASList/Player.baslist')
        # (protocol 29, tree mode) the armor puppets play the player's own AS too, in step with the avatar: their list
        # holds the player's defines as well, but not the files: Actor/AS/Player_*.bas are the player's own paths,
        # loaded with the player (resources are cached by path) and in the avatar's pack.
        if not slim:
            for f in player.get_files():
                if f.name.startswith('Actor/AS/'):
                    w.files[f.name] = bytes(f.data)
        # The player's own AS are decision trees fed by the player code (speed, angles...); the avatar plays these
        # single-clip ones instead (runtime/coop.c COOP_ANIM_* -> Coop<Name>), one clip of Player_Animation each.
        template = aamp(player, 'Actor/AS/Player_SquatBowWait.bas')  # (one TypeIndex 67 element: FileName, Morph)
        # The game looks AS names up in the list by binary search: the defines stay sorted by name.
        defines = aslist.lists['ASDefines'].objects
        # (protocol 29) The player's Face* / Voice* AS (face slot 3) are renamed CoopX<name> on the avatar and its
        # puppets: the player's AS start them with AS events by name (Nml_Wait -> "FaceDefault"), which on this NPC
        # class land in the upper-body slot 1 (seen 2026-11-10) and freeze the arms. Renamed, the events find nothing;
        # runtime/coop.c plays them as CoopX<name> in slot 3 itself.
        rows = [(('CoopX' + n) if n.startswith(('Face', 'Voice')) else n, f) for n, f in
                ((str(o.params['Name'].v), str(o.params['Filename'].v)) for _, o in defines.items())]
        for as_name, clip in COOP_CLIPS:
            el = template.lists['Elements'].lists['Element0'].objects['Parameters']
            el.params['FileName'] = oead.aamp.Parameter(oead.FixedSafeString64(clip))
            w.files[f'Actor/AS/Player_{as_name}.bas'] = template.to_binary()
            rows.append((as_name, f'Player_{as_name}'))
        for as_name, data in eat_face_files().items():  # (protocol 81) eat faces (files in the avatar's pack)
            if not slim:
                w.files[f'Actor/AS/Player_{as_name}.bas'] = data
            rows.append((as_name, f'Player_{as_name}'))
        if slim and ARM_WAIT:
            # (2026-11-18, guard-down: "the head comes off") Player_Wait.bas plays the transition
            # Nml_LockonBattleToWait after WaitAttention / CutS1 only when a bone test (Skl_Root,Z / Toe_R,Z) says so.
            # The head / upper armor puppets have no Toe_R and always took it, while the avatar's body stood up with
            # the plain idle (and in a later run the other way round). The puppets' "Wait" is a copy whose bone test
            # never picks the transition, "CoopArmWaitT" one that always does: runtime/coop/astree.inc (clip match)
            # plays CoopArmWaitT on a puppet when the avatar's body took the transition.
            # --no-arm-wait: the player's own Player_Wait.bas.
            for as_name, keep in (('Wait', 'Child1'), ('CoopArmWaitT', 'Child0')):
                wait = aamp(player, 'Actor/AS/Player_Wait.bas')
                els = [e for _, e in wait.lists['Elements'].lists.items()]
                fixed = 0
                for e in els:
                    ch = e.objects.get('Children') if 'Children' in e.objects else None
                    if int(e.objects['Parameters'].params['TypeIndex'].v) != 44 or ch is None or len(ch.params) != 2:
                        continue
                    c0 = int(ch.params['Child0'].v)
                    if 'LockonBattleToWait' in str(els[c0].objects['Parameters'].params.get('FileName', oead.aamp.Parameter('')).v):
                        both = ch.params[keep]  # (a copy keeps the Int type; Parameter(int) is F32)
                        ch.params['Child0'] = ch.params['Child1'] = both
                        fixed += 1
                if not fixed:
                    continue
                fn = 'Player_CoopArmWait' + ('T' if as_name != 'Wait' else '')
                w.files[f'Actor/AS/{fn}.bas'] = wait.to_binary()
                if as_name == 'Wait':
                    rows = [(n, fn if n == 'Wait' else f) for n, f in rows]
                else:
                    rows.append((as_name, fn))
        rows.sort(key=lambda r: r[0].encode())
        defines.clear()
        for i, (nm, fn) in enumerate(rows):
            d = oead.aamp.ParameterObject()
            d.params['Name'] = oead.aamp.Parameter(oead.FixedSafeString64(nm))
            d.params['Filename'] = oead.aamp.Parameter(oead.FixedSafeString64(fn))
            defines[f'ASDefine_{i}'] = d
        w.files[f'Actor/ASList/{as_user}.baslist'] = aslist.to_binary()
    elif bow_as is not None:
        # The real bow's limbs and string have their own draw and release clips.
        # Keep the puppet's dummy AI and use an independent animation list.
        bow, source = bow_as
        own_as = 'CoopW_' + source
        setp(t, 'ASUser', own_as)
        aslist = bow.get_file(f'Actor/ASList/{source}.baslist')
        w.files[f'Actor/ASList/{own_as}.baslist'] = bytes(aslist.data)
        for resource in bow.get_files():
            if resource.name.startswith('Actor/AS/'):
                w.files[resource.name] = bytes(resource.data)
    else:
        w.files['Actor/ASList/DemoDummy.baslist'] = bytes(stole.get_file('Actor/ASList/DemoDummy.baslist').data)
    # (2026-11-14) NOT the player's bone control (BoneControlUser = a copy of Player_Link.bbonectrl) on the avatar:
    # tried for the bow aim, both games crashed (access at 0x188) right after the avatar spawned, also with the aim
    # code off. runtime/coop/aim.inc adds its own bone modifier instead.
    if name in (BODY_ACTOR, HORSE_BODY_ACTOR) and AVATAR_PHYSICS:
        # (2026-11-11: collision with the other player) A body for the avatar, so the local
        # player bumps into the other player: one Keyframed capsule (r 0.4, y 0..1.9) copied from the mannequins'
        # Mannequin.bphysics. Its own name (resources are cached by path). runtime/coop/objects.inc collide_frame moves
        # it with the avatar (or parks it out of the way: stale data, teleports, climbing...).
        # Its own actor WITHOUT a model (2026-11-11 afternoon): on the avatar itself the model follows the body, so
        # the avatar's body, head and gear faced +z (the body had no rotation) and sat 60 m down while the body was
        # parked, away from its armor puppets (otherwise the other Link's head and body parts drift apart).
        # (2026-11-14: collision between the horses) Dm_CoopHorseBody: the same, with a horse-
        # sized capsule lying along the horse (horse_body_physics); runtime/coop/horse.inc horse_body_frame.
        phys = 'CoopHorseBody' if name == HORSE_BODY_ACTOR else 'CoopLinkE'
        setp(t, 'PhysicsUser', phys)
        setp(t, 'ModelUser', 'Dummy')
        setp(t, 'ElinkUser', 'Dummy')
        setp(t, 'ASUser', 'Dummy')
        for f in ('Actor/ModelList/PlayerStole2.bmodellist', 'Actor/ASList/DemoDummy.baslist'):
            if f in w.files:
                del w.files[f]
        w.files[f'Actor/Physics/{phys}.bphysics'] = HORSE_BODY_PHYSICS if name == HORSE_BODY_ACTOR else AVATAR_PHYSICS
    if name == STEP_ACTOR:
        # (2026-11-14) The avatar's footsteps: only the player's footstep sound set (SLink user PlayerFootStep), no
        # model, no effects. runtime/coop/avatarsnd.inc puts it at the avatar's foot and fires "PlayerFootStep".
        setp(t, 'ElinkUser', 'Dummy')
        setp(t, 'SlinkUser', 'PlayerFootStep')
    if name in (GLIDEFX_ACTOR, STEP_ACTOR):
        # (2026-11-11: the little wind effect was missing while the other player glides) Only the paraglider's
        # effect set (ElinkUser / ActorInfo elink PlayerStole2, kept from Dm_PlayerStole2), no model: the game has
        # model-less effect actors too (EfLocater_*). runtime/coop/avatar.inc glidefx_frame creates it only while the
        # other player glides (on the avatar it also trailed a walking avatar).
        setp(t, 'ModelUser', 'Dummy')
        setp(t, 'ASUser', 'Dummy')
        for f in ('Actor/ModelList/PlayerStole2.bmodellist', 'Actor/ASList/DemoDummy.baslist'):
            if f in w.files:
                del w.files[f]
    if name == HAIR_ACTOR and HAIR_CLOTH:
        # (2026-11-18) Link's hair strands are Havok cloth (Armor_Default_Head.bphysics: cloth only, 11 strands on
        # Hair_Root): the hair puppet gets the same cloth so the fringe and pony tail sway instead of hanging stiff.
        real = HAIR_ACTOR[len('Dm_CoopA_'):]
        rp = oead.Sarc(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'Pack', f'{real}.sbactorpack'), 'rb').read()))
        cloth_physics(w, t, rp, aamp(rp, f'Actor/ActorLink/{real}.bxml').objects['LinkTarget'], real)
    w.files[f'Actor/ActorLink/{name}.bxml'] = link.to_binary()
    w.files[f'Actor/GeneralParamList/{name}.bgparamlist'] = gparam.to_binary()
    _, data = w.write()
    os.makedirs(os.path.join(OUT, 'Actor', 'Pack'), exist_ok=True)
    with open(os.path.join(OUT, 'Actor', 'Pack', f'{name}.sbactorpack'), 'wb') as f:
        f.write(oead.yaz0.compress(data))
    return len(data)


BOOM_ACTOR = 'Dm_CoopBoom'


def make_boom():
    """(protocol 33, runtime/coop/pvp.inc pvp_boom) The other player's remote bomb blowing up, seen in our game: a copy
    of BarrelBomb whose AI goes straight to its explosion (Root -> 爆発: BombExplode + a 16-frame timer, then Delete)
    and whose physics has no attack sensor (AtkExplode removed: it hurts nothing here, the runtime applies the blast's
    damage itself) and a body that touches nothing (EntityNoHit, keyframed). Effects / sounds stay BarrelBomb's."""
    src = oead.Sarc(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'Pack', 'BarrelBomb.sbactorpack'), 'rb').read()))
    w = oead.SarcWriter.from_sarc(src)
    link = aamp(src, 'Actor/ActorLink/BarrelBomb.bxml')
    t = link.objects['LinkTarget']
    setp(t, 'AIProgramUser', 'CoopBoom')  # (own names for the changed files: resources are cached by path)
    setp(t, 'PhysicsUser', 'CoopBoom')
    ai = aamp(src, 'Actor/AIProgram/BarrelBomb.baiprog')
    if os.environ.get('COOP_BOOM_DIRECT') == '1':  # (tried first, 2026-11-11: no blast, the barrel just stood there)
        root = ai.lists['AI'].lists['AI_0']
        assert str(root.objects['Def'].params['Name'].v) == 'Root'
        idx = root.objects['ChildIdx']
        key = [k for k, v in idx.params.items() if int(v.v) == 3][0]  # (通常 -> AI_3, the BarrelBomb class)
        idx.params[key] = oead.aamp.Parameter(oead.S32(4))                      # (-> AI_4, 爆発)
    ph = aamp(src, 'Actor/Physics/BarrelBomb.bphysics')
    ps = ph.lists['ParamSet']
    sets = ps.lists['RigidBodySet'].lists
    for k in list(sets.keys()):
        if any(str(o.params['set_name'].v) == 'Atk' for o in sets[k].objects.values() if 'set_name' in o.params):
            del sets[k]
    head = [o for o in ps.objects.values() if 'use_rigid_body_set_num' in o.params][0]
    head.params['use_rigid_body_set_num'] = oead.aamp.Parameter(oead.S32(len(sets)))
    for s in sets.values():
        for rb in s.lists.values():
            for o in rb.objects.values():
                if 'layer' in o.params and str(o.params['layer'].v) == 'EntityObject':
                    setp(o, 'layer', 'EntityNoHit')
                    setp(o, 'motion_type', 'Keyframed')
                    setp(o, 'navmesh', 'NOT_USE')
    for f in ('Actor/ActorLink/BarrelBomb.bxml', 'Actor/AIProgram/BarrelBomb.baiprog', 'Actor/Physics/BarrelBomb.bphysics'):
        del w.files[f]
    w.files[f'Actor/ActorLink/{BOOM_ACTOR}.bxml'] = link.to_binary()
    w.files['Actor/AIProgram/CoopBoom.baiprog'] = ai.to_binary()
    w.files['Actor/Physics/CoopBoom.bphysics'] = ph.to_binary()
    _, data = w.write()
    with open(os.path.join(OUT, 'Actor', 'Pack', f'{BOOM_ACTOR}.sbactorpack'), 'wb') as f:
        f.write(oead.yaz0.compress(data))
    return len(data)


HORSE_PREFIX = 'Dm_CoopH_'


def horse_body_for(acc):
    """The horse body whose AS list an accessory (mane, saddle, reins) plays: the large / donkey ones have their own."""
    if '00L' in acc:
        return 'GameRomHorse00L'
    if '00S' in acc:
        return 'GameRomHorse00S'
    return 'GameRomHorse01'


HAIR_ACTOR = 'Dm_CoopA_Armor_Default_Head'  # (the avatar's hair: Link's bare-head 'armor')
HAIR_CLOTH = '--no-hair-cloth' not in sys.argv  # (off switch: hair puppet without its cloth sway)
ARM_WAIT = '--no-arm-wait' not in sys.argv  # (the armor puppets' "Wait" without the bone-tested transition)
HORSE_CLOTH = '--no-horse-cloth' not in sys.argv  # (off switch: puppets without swaying hair/reins)


def cloth_physics(w, t, rp, rl, real):
    """(2026-11-14) The mane (mane, forelock and tail: Horse_Link_Mane*), reins and some saddles sway with Havok cloth
    (Actor/Physics/<x>.bphysics 'Cloth' list + Physics/Cloth/<x>/<x>.hkcl). A puppet with PhysicsUser Dummy hangs
    stiff. Give it a cloth-only copy (no rigid bodies, ragdoll, character controller or contact) under its own name;
    the hkcl keeps its path (the same file, so sharing it in the resource cache is harmless)."""
    pu = str(rl.params['PhysicsUser'].v)
    if pu == 'Dummy' or f'Actor/Physics/{pu}.bphysics' not in [f.name for f in rp.get_files()]:
        return
    pio = aamp(rp, f'Actor/Physics/{pu}.bphysics')
    ps = pio.lists['ParamSet']
    flags = next(iter(ps.objects.values()))
    if not flags.params['use_cloth'].v or 'Cloth' not in ps.lists:
        return
    for k, v in (('use_rigid_body_set_num', 0), ('use_ragdoll', False), ('use_support_bone', False),
                 ('use_character_controller', False), ('use_contact_info', False), ('use_edge_rigid_body_num', 0)):
        if k in flags.params:
            setp(flags, k, v)
    for k in list(ps.lists.keys()):
        if k != oead.aamp.Name('Cloth'):
            del ps.lists[k]
    own = f'CoopH_{pu}'
    setp(t, 'PhysicsUser', own)
    w.files[f'Actor/Physics/{own}.bphysics'] = pio.to_binary()
    for f in rp.get_files():
        if f.name.startswith('Physics/Cloth/'):
            w.files[f.name] = bytes(f.data)


def make_horse_pack(stole, real, body):
    """(2026-11-14) The other player's horse (runtime/coop/horse.inc): Dm_CoopH_<real> is a physics-free puppet (AI
    DemoDummy, profile NPC, like the avatar) with the real actor's model and the horse body's AS list and AS files, so
    it plays the horse's own AS (Move_Gear2, Jump_Gear3...) sent by the other game. The real horse is a Horse actor
    with AI and physics; the puppet only shows it. Mane / saddle / reins are separate actors in the game (bound to the
    horse's skeleton in code): each gets a puppet of its own with the BODY's AnmTarget and AS list, playing the same AS
    in the same frame (like the armor puppets on the avatar). real: e.g. GameRomHorse01, Horse_Link_Mane_03,
    GameRomHorseSaddle_01; body: the body pack whose AS list it plays."""
    name = HORSE_PREFIX + real
    rp = oead.Sarc(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'Pack', f'{real}.sbactorpack'), 'rb').read()))
    bp = oead.Sarc(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'Pack', f'{body}.sbactorpack'), 'rb').read()))
    rl = aamp(rp, f'Actor/ActorLink/{real}.bxml').objects['LinkTarget']
    bl = aamp(bp, f'Actor/ActorLink/{body}.bxml').objects['LinkTarget']
    model_user, as_user = str(rl.params['ModelUser'].v), str(bl.params['ASUser'].v)
    body_model_user = str(bl.params['ModelUser'].v)
    w = oead.SarcWriter.from_sarc(stole)
    w.files.clear()
    link = aamp(stole, 'Actor/ActorLink/Dm_PlayerStole2.bxml')
    t = link.objects['LinkTarget']
    setp(t, 'GParamUser', name)
    setp(t, 'ProfileUser', 'NPC')  # (the avatar's class: the NPC input hook keeps our AS inputs, coop/astree.inc)
    setp(t, 'ElinkUser', 'Dummy')
    own_model, own_as = f'CoopH_{real}', f'CoopH_{as_user}'  # (own names: resources are cached by path)
    mane_collar = real != body and real.startswith('Horse_Link_Mane')
    if mane_collar:  # (its own list: the body's + the mane's Collar* AS, below)
        own_as = f'CoopH_{as_user}_{real}'
    setp(t, 'ModelUser', own_model)
    setp(t, 'ASUser', own_as)
    gparam = aamp(stole, 'Actor/GeneralParamList/Dm_PlayerStole2.bgparamlist')
    setp(gparam.objects['Event'], 'VisibleOffActor1', '')
    w.files['Actor/AIProgram/DemoDummy.baiprog'] = bytes(stole.get_file('Actor/AIProgram/DemoDummy.baiprog').data)
    models = aamp(rp, f'Actor/ModelList/{model_user}.bmodellist')
    setp(models.objects['Attention'], 'IsEnableAttention', False)  # (no lock-on target, no "talk" prompt)
    if real != body:  # an accessory: the body's animation targets (slots), so the body's AS bind to its bones
        bm = aamp(bp, f'Actor/ModelList/{body_model_user}.bmodellist')
        models.lists['AnmTarget'] = bm.lists['AnmTarget']
    w.files[f'Actor/ModelList/{own_model}.bmodellist'] = models.to_binary()
    if mane_collar:
        # (2026-11-14, horse coat) The mane's colour: the real mane plays its own "Collar[_Coward...]" AS (shader
        # params, Horse_Link_Mane_Animation) at the body's coat frame (runtime/coop/horse.inc horse_coat). The puppet
        # gets the body's AS list plus the mane's Collar* defines (sorted by name: the game binary-searches them), the
        # mane's anim resources and its Collar AS files.
        aslist = aamp(bp, f'Actor/ASList/{as_user}.baslist')
        ml = aamp(rp, f'Actor/ASList/{str(rl.params["ASUser"].v)}.baslist')
        reses = aslist.lists['AddReses'].objects
        have = {str(o.params['Anim'].v) for _, o in reses.items()}
        for _, o in ml.lists['AddReses'].objects.items():
            if str(o.params['Anim'].v) not in have:
                reses[f'AddRes_{len(reses)}'] = o
        defines = aslist.lists['ASDefines'].objects
        rows = [(str(o.params['Name'].v), str(o.params['Filename'].v)) for _, o in defines.items()]
        rows += [(str(o.params['Name'].v), str(o.params['Filename'].v)) for _, o in ml.lists['ASDefines'].objects.items()
                 if str(o.params['Name'].v).startswith('Collar')]
        rows.sort(key=lambda r: r[0].encode())
        defines.clear()
        for i, (nm, fn) in enumerate(rows):
            d = oead.aamp.ParameterObject()
            d.params['Name'] = oead.aamp.Parameter(oead.FixedSafeString64(nm))
            d.params['Filename'] = oead.aamp.Parameter(oead.FixedSafeString64(fn))
            defines[f'ASDefine_{i}'] = d
        w.files[f'Actor/ASList/{own_as}.baslist'] = aslist.to_binary()
        for f in rp.get_files():
            if f.name.startswith('Actor/AS/') and '_Collar' in f.name:
                w.files[f.name] = bytes(f.data)
    else:
        w.files[f'Actor/ASList/{own_as}.baslist'] = bytes(bp.get_file(f'Actor/ASList/{as_user}.baslist').data)
    for f in bp.get_files():
        if f.name.startswith('Actor/AS/'):
            w.files[f.name] = bytes(f.data)
    if real != body and HORSE_CLOTH:
        cloth_physics(w, t, rp, rl, real)
    w.files[f'Actor/ActorLink/{name}.bxml'] = link.to_binary()
    w.files[f'Actor/GeneralParamList/{name}.bgparamlist'] = gparam.to_binary()
    _, data = w.write()
    os.makedirs(os.path.join(OUT, 'Actor', 'Pack'), exist_ok=True)
    with open(os.path.join(OUT, 'Actor', 'Pack', f'{name}.sbactorpack'), 'wb') as f:
        f.write(oead.yaz0.compress(data))
    return len(data)


def horse_variants():
    """(real actor, body pack) for every horse body, mane (not the Grabbed ones: wild animals held by the mane) saddle
    and reins in the player's files."""
    out = []
    for f in sorted(os.listdir(os.path.join(GAME, 'Actor', 'Pack'))):
        n = f[:-len('.sbactorpack')] if f.endswith('.sbactorpack') else ''
        if re.match(r'^GameRomHorse(?!Saddle_|Reins_)\w+$', n) or n == 'GameRomHorse':
            out.append((n, n))
        elif re.match(r'^(Horse_Link_Mane(_\w+)?|GameRomHorseSaddle_\w+|GameRomHorseReins_\w+)$', n) and 'Grabbed' not in n:
            out.append((n, horse_body_for(n)))
        elif MOUNTS and re.match(MOUNT_RE, n):
            out.append((n, n))
    return out


# (2026-11-19, coop-horse2) Other mounts the partner rides, shown the same way (runtime/coop/horse.inc mount kinds):
# sand seals, Prince Sidon, the Master Cycle, and the wild deer / does / bears a player can jump on. Each puppet has
# the real actor's model and its own AS list. --no-mounts: none of these.
MOUNTS = '--no-mounts' not in sys.argv
MOUNT_RE = r'^(Animal_Sunazarashi\w*|Npc_ZoraB001|GameROMMotorcycle|Animal_(Deer|Doe|Bear)_[A-Z])$'


AVATAR_PHYSICS = None  # (main: the avatar's body, from Mannequin.bphysics)
HORSE_BODY_ACTOR = 'Dm_CoopHorseBody'  # (2026-11-14: the partner's horse's collision body; runtime/coop/horse.inc)
HORSE_BODY_PHYSICS = None  # (main: horse_body_physics())
BODY_ACTOR = 'Dm_CoopBody'  # (the avatar's collision body: physics only, no model; runtime/coop/objects.inc)
GLIDEFX_ACTOR = 'Dm_CoopGlideFx'  # (the paraglider's wind while the avatar glides: effects only; runtime/coop/avatar.inc)
AVATAR_FX_ACTOR = 'Dm_CoopLinkF'  # (2026-11-14: Dm_CoopLinkE with the player's effect set Player_GameRom; runtime/coop/avatarfx.inc)
AVATAR_SND_ACTOR = 'Dm_CoopLinkS'  # (2026-11-14: Dm_CoopLinkF + the player's sound set GameROMPlayer; runtime/coop/avatarsnd.inc)
STEP_ACTOR = 'Dm_CoopStep'  # (2026-11-14: the avatar's footsteps: sound set PlayerFootStep only; runtime/coop/avatarsnd.inc)
PUPPET_SLINK = {}  # (main) weapon puppet name -> its real weapon's ActorLink SlinkUser
ELINK_WEAPONS = True
PUPPET_ELINK = {}  # (main) melee weapon puppet name -> its real weapon's ActorLink ElinkUser (2026-11-16)


def v3(x, y, z):
    v = oead.Vector3f()
    v.x, v.y, v.z = x, y, z
    return v


def horse_body_physics():
    """The avatar body's Keyframed capsule (layer EntityObject) made horse-sized: lying along +z (the horse's forward),
    0.55 m thick, from the chest (z 1.1) to the rump (z -0.8) at 1.25 m (a real horse's Body capsule: r 0.5, 2.3 m)."""
    pio = oead.aamp.ParameterIO.from_binary(avatar_physics())
    rb = pio.lists['ParamSet'].lists['RigidBodySet'].lists['RigidBodySet_0'].lists['RigidBody_0']
    body = rb.objects[oead.aamp.Name(948250248)]
    body.params['center_of_mass'] = oead.aamp.Parameter(v3(0.0, 1.25, 0.15))
    body.params['bounding_center'] = oead.aamp.Parameter(v3(0.0, 1.25, 0.15))
    body.params['bounding_extents'] = oead.aamp.Parameter(v3(1.1, 1.1, 3.0))
    sh = rb.objects['ShapeParam_0']
    sh.params['translate_0'] = oead.aamp.Parameter(v3(0.0, 1.25, 1.1))
    sh.params['translate_1'] = oead.aamp.Parameter(v3(0.0, 1.25, -0.8))
    sh.params['radius'] = oead.aamp.Parameter(0.55)
    return pio.to_binary()


def avatar_physics():
    """Mannequin.bphysics (one Keyframed capsule, layer EntityObject) with navmesh cutting off."""
    mq = oead.Sarc(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'Pack', 'Mannequin_001_Head.sbactorpack'), 'rb').read()))
    pio = oead.aamp.ParameterIO.from_binary(bytes(mq.get_file('Actor/Physics/Mannequin.bphysics').data))
    rb = pio.lists['ParamSet'].lists['RigidBodySet'].lists['RigidBodySet_0'].lists['RigidBody_0']
    body = rb.objects[oead.aamp.Name(948250248)]
    setp(body, 'navmesh', 'NOT_USE')  # (a moving player must not cut the nav mesh enemies walk on)
    return pio.to_binary()


def main():
    global GAME, OUT, AVATAR_PHYSICS, HORSE_BODY_PHYSICS
    ap = argparse.ArgumentParser()
    ap.add_argument('--play', action='store_true')
    ap.add_argument('--game', default=GAME)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--no-horse-cloth', action='store_true')  # (see HORSE_CLOTH)
    ap.add_argument('--no-mounts', action='store_true')  # (see MOUNTS)
    ap.add_argument('--no-hair-cloth', action='store_true')  # (see HAIR_CLOTH)
    ap.add_argument('--no-arm-wait', action='store_true')  # (see ARM_WAIT)
    ap.add_argument('--no-weapon-elink', action='store_true')  # (2026-11-16: weapon puppets without effect sets)
    args = ap.parse_args()
    GAME, OUT = args.game, args.out
    global ELINK_WEAPONS
    ELINK_WEAPONS = not args.no_weapon_elink
    AVATAR_PHYSICS = avatar_physics()
    HORSE_BODY_PHYSICS = horse_body_physics()
    title =oead.Sarc(open(os.path.join(GAME, 'Pack', 'TitleBG.pack'), 'rb').read())
    player = oead.Sarc(oead.yaz0.decompress(bytes(title.get_file('Actor/Pack/GameROMPlayer.sbactorpack').data)))
    stole = oead.Sarc(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'Pack', 'Dm_PlayerStole2.sbactorpack'), 'rb').read()))
    # (name, model list: False | (folder, unit) | 'player', player AS in the pack, ActorInfo entry: 'stole' | 'model' |
    # 'player'); 'model' sets the entry's bfres/mainModel to the model list's folder/unit (Link for 'player')
    link = ('Link', 'Link')
    # The avatar's clothes: the Hylian set (Armor_001) as extra ModelData on the body model. runtime/coop.c hides the
    # body parts each piece covers with the material-group function, using the model slots at +0x30/+0x40/+0x50.
    outfit = [link, ('Armor_001', 'Armor_001_Head'), ('Armor_001', 'Armor_001_Upper'), ('Armor_001', 'Armor_001_Lower')]
    variants = [('Dm_CoopTestA', False, False, 'stole'), ('Dm_CoopTestB1', 'player', False, 'model'),
                ('Dm_CoopTestB2', False, False, 'player'), ('Dm_CoopTestB3', link, False, 'model'),
                ('Dm_CoopTestB4', link, False, 'stole'), ('Dm_CoopTestB5', ('Enemy_Bokoblin', 'Bokoblin_Red'), False, 'model'),
                ('Dm_CoopTestB6', ('Armor_001', 'Armor_001_Upper'), False, 'model'),
                ('Dm_CoopTestB7', outfit, False, 'model'),
                ('Dm_CoopLinkC', link, True, 'model'),
                ('Dm_CoopLinkD', link, True, 'model', 'PlayerItem'), ('Dm_CoopLinkE', link, True, 'model', 'NPC'),
                (AVATAR_FX_ACTOR, link, True, 'model', 'NPC'),
                (AVATAR_SND_ACTOR, link, True, 'model', 'NPC'),
                ('Dm_CoopLink', 'player', True, 'player'),
                # The sword in the avatar's hand while it swings (runtime/coop.c moves it to the avatar's Weapon_R bone)
                ('Dm_CoopSword', ('Weapon_Sword_001', 'Weapon_Sword_001'), False, 'model', 'NPC', 'CoopSword')]
    # One puppet per melee weapon (Dm_CoopW_<Sword_044>...): runtime/coop.c shows the one the other player holds while
    # swinging (Dm_CoopSword when it has none for that weapon). Model folder/unit from each weapon's own model list.
    # (protocol 14: shields and bows too, shown in the hand or on the back; the paraglider while gliding)
    variants.append(('Dm_CoopGlider', False, False, 'stole', 'NPC'))
    variants.append((BODY_ACTOR, False, False, 'stole', 'NPC'))
    variants.append((HORSE_BODY_ACTOR, False, False, 'stole', 'NPC'))
    variants.append((GLIDEFX_ACTOR, False, False, 'stole', 'NPC'))
    variants.append((STEP_ACTOR, False, False, 'stole', 'NPC'))
    powers = []
    bow_animations = {}
    for f in sorted(os.listdir(os.path.join(GAME, 'Actor', 'Pack'))):
        m = re.match(r'^Weapon_((?:Sword|Lsword|Spear|Shield|Bow)_\d+)\.sbactorpack$', f)
        if not m:
            continue
        wp = oead.Sarc(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'Pack', f), 'rb').read()))
        # Friendly damage (runtime/coop/pvp.inc): each melee weapon's attack power, read by the runtime from
        # coop_weapon_power.txt in the overlay folder (from the player's own files, like the rest of the overlay).
        gp = [x for x in wp.get_files() if x.name.startswith('Actor/GeneralParamList/')]
        # (protocol 35: bows too - an arrow hitting the other player does the bow's power, runtime/coop/arrows.inc)
        if gp and re.match(r'^(Sword|Lsword|Spear|Bow)_', m.group(1)):
            try:
                pw = int(oead.aamp.ParameterIO.from_binary(bytes(gp[0].data)).objects['Attack'].params['Power'].v)
                powers.append(f'Weapon_{m.group(1)} {pw}')
            except (KeyError, ValueError, TypeError):
                pass
        ml = [x for x in wp.get_files() if x.name.startswith('Actor/ModelList/')]
        if not ml:
            continue
        try:
            md = oead.aamp.ParameterIO.from_binary(bytes(ml[0].data)).lists['ModelData'].lists['ModelData_0']
            folder = str(md.objects['Base'].params['Folder'].v)
            unit = str(md.lists['Unit'].objects['Unit_0'].params['UnitName'].v)
        except KeyError:
            continue  # (a few weapons have no plain model list: they get the Traveler's Sword)
        if not folder or not unit:
            continue
        puppet = f'Dm_CoopW_{m.group(1)}'
        variants.append((puppet, (folder, unit), False, 'model', 'NPC', f'CoopW_{m.group(1)}'))
        al = [x for x in wp.get_files() if x.name.startswith('Actor/ActorLink/')]
        try:  # (2026-11-14) its sound set, for the swing sounds (runtime/coop/avatarsnd.inc)
            target = oead.aamp.ParameterIO.from_binary(bytes(al[0].data)).objects['LinkTarget'].params
            su = str(target['SlinkUser'].v)
            if su and su != 'Dummy':
                PUPPET_SLINK[puppet] = su
            eu = str(target['ElinkUser'].v)
            if eu and eu != 'Dummy' and re.match(r'^(Sword|Lsword|Spear)_', m.group(1)) and ELINK_WEAPONS:
                PUPPET_ELINK[puppet] = eu
            if m.group(1).startswith('Bow_'):
                source = str(target['ASUser'].v)
                if wp.get_file(f'Actor/ASList/{source}.baslist') is not None:
                    bow_animations[puppet] = (wp, source)
        except (KeyError, IndexError):
            pass
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'coop_weapon_power.txt'), 'w') as f:
        f.write('\n'.join(powers) + '\n')
    # (2026-11-15) Each enemy's own Attack Power (its unarmed blows hitting the other player: runtime/coop/pvp.inc
    # foe_unarmed reads coop_enemy_power.txt). Bokoblin_Junior 4, Chuchu_Junior 2.
    epowers = []
    for f in sorted(os.listdir(os.path.join(GAME, 'Actor', 'Pack'))):
        if not (f.startswith('Enemy_') and f.endswith('.sbactorpack')):
            continue
        try:
            ep = oead.Sarc(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'Pack', f), 'rb').read()))
            gp = [x for x in ep.get_files() if x.name.startswith('Actor/GeneralParamList/')]
            if gp:
                pw = int(oead.aamp.ParameterIO.from_binary(bytes(gp[0].data)).objects['Attack'].params['Power'].v)
                epowers.append(f'{f[:-len(".sbactorpack")]} {pw}')
        except (KeyError, ValueError, TypeError, RuntimeError):
            pass
    with open(os.path.join(OUT, 'coop_enemy_power.txt'), 'w') as f:
        f.write('\n'.join(epowers) + '\n')
    # Carried props (protocol 21: remote bombs, barrels, pots, rocks carried overhead)
    carried_props = [
        ('Dm_CoopW_RemoteBomb', ('Item_RemoteBomb', 'Item_RemoteBomb')),
        ('Dm_CoopW_RemoteBombCube', ('Item_RemoteBombCube', 'Item_RemoteBombCube')),
        ('Dm_CoopW_RemoteBomb2', ('Item_RemoteBomb', 'Item_RemoteBomb')),
        ('Dm_CoopW_RemoteBombCube2', ('Item_RemoteBombCube', 'Item_RemoteBombCube')),
        ('Dm_CoopW_Barrel', ('Obj_Barrel_A', 'Obj_Barrel_A')),
        ('Dm_CoopW_BarrelBomb', ('Obj_BarrelBomb_A', 'Obj_BarrelBomb_A_01')),
        ('Dm_CoopW_Pot', ('Obj_Pot_A', 'Obj_Pot_A_01')),
        ('Dm_CoopW_KorokStone', ('FldObj_KorokStone_A', 'FldObj_KorokStone_A_03')),
        ('Dm_CoopW_FldObj_KorokStoneLift_A_01', ('FldObj_KorokStone_A', 'FldObj_KorokStone_A_03')),
        # (2026-11-14: the arrow did not show) the arrow on the string while the partner aims
        ('Dm_CoopW_NormalArrow', ('Weapon_Arrow_001', 'Weapon_Arrow_001')),
        ('Dm_CoopW_BombArrow_A', ('Weapon_Arrow_002', 'Weapon_Arrow_002')),
        ('Dm_CoopW_ElectricArrow', ('Weapon_Arrow_004', 'Weapon_Arrow_004')),
        ('Dm_CoopW_FireArrow', ('Weapon_Arrow_005', 'Weapon_Arrow_005')),
        ('Dm_CoopW_IceArrow', ('Weapon_Arrow_006', 'Weapon_Arrow_006')),
        ('Dm_CoopW_AncientArrow', ('Weapon_Arrow_007', 'Weapon_Arrow_007')),
        # (2026-11-15, protocol 52) enemy shots flying in the other game (shots.inc shots_view)
        ('Dm_CoopW_OctRock', ('Enemy_Octarock', 'Octarock_Rock')),
        ('Dm_CoopW_OctRockSnow', ('Enemy_Octarock', 'Octarock_RockSnow')),
        ('Dm_CoopW_RockWeapon', ('Obj_Rock_Weapon', 'Rock_Weapon')),
        ('Dm_CoopW_GiantRock', ('Enemy_Giant', 'Enemy_Giant_Stone')),
    ]
    for p_name, p_model in carried_props:
        variants.append((p_name, p_model, False, 'model', 'NPC', p_name.replace('Dm_', '')))
    info = oead.byml.from_binary(oead.yaz0.decompress(open(os.path.join(GAME, 'Actor', 'ActorInfo.product.sbyml'), 'rb').read()))
    rows = [(int(h), a) for h, a in zip(info['Hashes'], info['Actors'])]
    byname = {a['name']: a for _, a in rows}
    # (protocol 49, 2026-11-15) the Sheikah Slate (actor Item_Conductor: hip / hand while a rune is used), and a model of
    # every item that can be held in the hands from the inventory (apples, materials, insects, fish...): runtime/coop/
    # avatar.inc shows the partner's held items on the avatar (Dm_CoopW_<actor name>).
    variants.append(('Dm_CoopW_Slate', ('Item_Conductor', 'Item_Conductor'), False, 'model', 'NPC', 'CoopW_Slate'))
    held_n = 0
    for an in sorted(byname):
        a = byname[an]
        if (str(a.get('profile', '')) not in ('Item', 'CapturedActor') or an == 'Item_Conductor' or
                not re.match(r'^(Item_|Animal_|BeeHome|Obj_)\w+$', an) or len(an) > 31):
            continue
        folder, unit = str(a.get('bfres', '')), str(a.get('mainModel', ''))
        if folder and unit and not any(v[0] == f'Dm_CoopW_{an}' for v in variants):
            variants.append((f'Dm_CoopW_{an}', (folder, unit), False, 'model', 'NPC', f'CoopW_{an}'))
            held_n += 1
    print(f'held item puppets: {held_n}')
    # (2026-11-19, play report: "picking up a half-broken barrel, the partner sees the avatar carrying a whole barrel")
    # The carried prop was chosen by name family (any *Barrel* -> Dm_CoopW_Barrel, the whole wooden barrel), so the
    # old rotten barrel Obj_BarrelOld_A_01 (its own, broken-looking model), stone barrels, hidden barrels, Korok pots and
    # crates showed the wrong model. Now each carriable map object gets a prop with its own model
    # (Dm_CoopW_<actor name>); runtime/coop/avatar.inc gear_puppet tries that exact name first (SAVAGEWIND_COOP_HELDSTATE).
    # (2026-11-20, seen in a test: a lifted rock - Obj_LiftRock*, e.g. a Korok's rock - had no prop at all: the avatar
    # held up empty hands) Lift rocks get their own prop too.
    # (2026-11-21, 11-20 evening play: a cucco lifted by the partner was invisible in their hands - Kakariko quest)
    # Cuccos (Animal_Kokko*) get one too.
    lift_n = 0
    for an in sorted(byname):
        a = byname[an]
        if (not re.match(r'^(Barrel\w*|Obj_BarrelOld\w*|Obj_HideBarrel|DgnObj_StoneBarrel\w*|Kibako\w*|'
                         r'DgnObj_KibakoDungeon\w*|Obj_KorokPot\w*|Pot|Obj_LiftRock\w*|Animal_Kokko\w*)$', an) or len(an) > 31 or 'Balloon' in an):
            continue
        folder, unit = str(a.get('bfres', '')), str(a.get('mainModel', ''))
        if folder and unit and not any(v[0] == f'Dm_CoopW_{an}' for v in variants):
            variants.append((f'Dm_CoopW_{an}', (folder, unit), False, 'model', 'NPC', f'CoopW_{an}'))
            lift_n += 1
    print(f'carried object props: {lift_n}')
    # One puppet per armor (Dm_CoopA_<Armor_001_Upper>): the armor's model (ActorInfo bfres/mainModel) with the Coop*
    # AS only; runtime/coop.c puts it on the avatar and plays the avatar's AS on it in the same frame.
    for an in sorted(byname):
        a = byname[an]
        if (not re.match(r'^Armor_\w+$', an) or an.startswith('Armor_Default') or  # (Default = no armor: bare Link)
                str(a.get('profile', '')) not in ('ArmorHead', 'ArmorUpper', 'ArmorLower')):
            continue
        folder, unit = str(a.get('bfres', '')), str(a.get('mainModel', ''))
        if folder and unit:
            variants.append((f'Dm_CoopA_{an}', (folder, unit), 'coop', 'model', 'NPC', f'CoopA_{an}'))
    # (2026-11-18: the other Links were always bald) Link's hair is the "no head armor" actor
    # Armor_Default_Head (model Armor_Default/Armor_Default_Head, in TitleBG.pack): runtime/coop/avatar.inc puts this
    # puppet on the avatar when the partner wears no head armor (SAVAGEWIND_COOP_HAIR=0 off).
    variants.append((HAIR_ACTOR, ('Armor_Default', 'Armor_Default_Head'), 'coop', 'model', 'NPC',
                     'CoopA_Armor_Default_Head'))
    # (2026-11-11: the avatar's ears went through the Hylian hood) A head armor folds Link's ears by
    # ArmorHead.EarRotate (degrees x y z; Armor_001_Head: 0 50 0) of its GeneralParamList: runtime/coop/avatar.inc
    # ears_frame turns the avatar's Ear_L / Ear_R bones by it, read from coop_ear_rotate.txt (from the player's files).
    ears = []
    for an in sorted(byname):
        if not re.match(r'^Armor_\w+$', an) or str(byname[an].get('profile', '')) != 'ArmorHead':
            continue
        p = os.path.join(GAME, 'Actor', 'Pack', f'{an}.sbactorpack')
        if not os.path.exists(p):
            continue
        try:
            ap = oead.Sarc(oead.yaz0.decompress(open(p, 'rb').read()))
            gp = [x for x in ap.get_files() if x.name.startswith('Actor/GeneralParamList/')]
            r = oead.aamp.ParameterIO.from_binary(bytes(gp[0].data)).objects['ArmorHead'].params['EarRotate'].v
            if abs(r.x) + abs(r.y) + abs(r.z) > 0.01:
                ears.append(f'{an} {r.x:g} {r.y:g} {r.z:g}')
        except (KeyError, IndexError, RuntimeError, ValueError):
            continue
    with open(os.path.join(OUT, 'coop_ear_rotate.txt'), 'w') as f:
        f.write('\n'.join(ears) + '\n')
    # (2026-11-20) play_coop.ps1 reruns setup when this stamp differs from its own (bump both together)
    with open(os.path.join(OUT, 'coop_overlay_version.txt'), 'w') as f:
        f.write(OVERLAY_VERSION + '\n')
    if args.play:
        variants = [v for v in variants if v[0] in ('Dm_CoopLinkE', AVATAR_FX_ACTOR, AVATAR_SND_ACTOR, STEP_ACTOR, 'Dm_CoopSword',
                                                    'Dm_CoopGlider', BODY_ACTOR, HORSE_BODY_ACTOR, GLIDEFX_ACTOR) or
                    v[0].startswith(('Dm_CoopW_', 'Dm_CoopA_'))]
    sizes = {v[0]: make_pack(stole, player, v[0], v[1], v[2], v[4] if len(v) > 4 else None,
                             v[5] if len(v) > 5 else 'CoopLink', bow_as=bow_animations.get(v[0])) for v in variants}

    # ActorInfo: entries copied from the puppet's (Link's model where the pack has it); sorted by crc32 of the name
    src = byname['GameROMPlayer']
    for v in variants:
        name, model, _, entry = v[:4]
        e = oead.byml.Hash(dict(byname['Dm_PlayerStole2']))
        e['name'] = name
        # (2026-11-11) Copied from Dm_PlayerStole2 (the paraglider), the entry named the paraglider's effect set
        # ("elink": PlayerStole2). On the avatar and the armor / gear puppets it drew thin white air-trail ribbons
        # behind a running avatar (a little puff of air). Only the paraglider puppet keeps it.
        if (entry != 'stole' or name in (BODY_ACTOR, HORSE_BODY_ACTOR)) and 'elink' in e:
            del e['elink']
        if name in (AVATAR_FX_ACTOR, AVATAR_SND_ACTOR):  # (2026-11-14) the player's effect set, and room in its heap for it
            e['elink'] = src['elink']
            if int(src['instSize']) > int(e['instSize']):
                e['instSize'] = oead.S32(int(src['instSize']))
        if name == AVATAR_SND_ACTOR:  # (2026-11-14) and its sound set
            e['slink'] = src['slink']
        if name == STEP_ACTOR:  # (2026-11-14) footstep sounds only (no paraglider effects)
            if 'elink' in e:
                del e['elink']
            e['slink'] = 'PlayerFootStep'
        if name in PUPPET_SLINK:  # (2026-11-14) the real weapon's sound set (ActorInfo slink, e.g. Weapon_Sword_Metal)
            wr = byname.get('Weapon_' + name[9:])
            if wr is not None and 'slink' in wr:
                e['slink'] = wr['slink']
        if name in PUPPET_ELINK:  # (2026-11-16) and its effect set (ActorInfo elink)
            wr = byname.get('Weapon_' + name[9:])
            if wr is not None and 'elink' in wr:
                e['elink'] = wr['elink']
        # Without a traverseDist the game deletes the avatar once it is ~60 m from the local player (seen 2026-11-07:
        # recreated every 3 s at 65 m). Enemies use 150, NPCs 200-500; 99999 is the game's own "never" value.
        e['traverseDist'] = oead.F32(99999.0)
        # (2026-11-13) The actor's own heap is instSize bytes. The paraglider's 86728 is too small for many armor /
        # weapon models (Armor_046_Head needs ~225 KB): the model setup then got a null block and the game crashed
        # (flaky: test_avatar_look, Divine Beast boss arenas). A puppet gets at least its real item's instSize.
        if name.startswith(('Dm_CoopA_', 'Dm_CoopW_')):
            base = name[9:]
            # (2026-11-14: weapon puppets are Dm_CoopW_<Sword_044>, the real actor Weapon_Sword_044; they now carry
            # a sound set too, so they need the real weapon's heap size)
            real = byname.get(base) or byname.get('Weapon_' + base) or byname.get(base.rsplit('_', 1)[0])
            if real is not None and 'instSize' in real and int(real['instSize']) > int(e['instSize']):
                e['instSize'] = oead.S32(int(real['instSize']))
        if len(v) > 4:
            e['profile'] = v[4]
        if entry in ('model', 'player'):
            folder, unit = model if isinstance(model, tuple) else model[0] if isinstance(model, list) else link
            e['bfres'] = folder
            e['mainModel'] = unit
        if entry == 'player':
            e['aabbMin'] = src['aabbMin']
            e['aabbMax'] = src['aabbMax']
            e['boundingForTraverse'] = src['boundingForTraverse']
            e['instSize'] = oead.S32(int(src['instSize']))
            if 'elink' in e:
                del e['elink']
        if HEAP_SCALE != 1.0 and 'instSize' in e:  # (see HEAP_SCALE)
            e['instSize'] = oead.S32(int(int(e['instSize']) * HEAP_SCALE))
        rows.append((zlib.crc32(name.encode()), e))
    sizes[BOOM_ACTOR] = make_boom()  # (protocol 33: the other player's bomb blast, BarrelBomb's entry renamed)
    e = oead.byml.Hash(dict(byname['BarrelBomb']))
    e['name'] = BOOM_ACTOR
    e['traverseDist'] = oead.F32(99999.0)
    rows.append((zlib.crc32(BOOM_ACTOR.encode()), e))
    # (2026-11-14) The other player's horse: body, mane, saddle and reins puppets (runtime/coop/horse.inc). Entry from
    # Dm_PlayerStole2's (DemoDummy puppet), with the real actor's model, bounds and (at least) its heap size.
    nh = 0
    for real, body in horse_variants():
        r = byname.get(real)
        if r is None or not str(r.get('bfres', '')) or not str(r.get('mainModel', '')):
            continue
        name = HORSE_PREFIX + real
        sizes[name] = make_horse_pack(stole, real, body)
        nh += 1
        e = oead.byml.Hash(dict(byname['Dm_PlayerStole2']))
        e['name'] = name
        if 'elink' in e:
            del e['elink']
        e['profile'] = 'NPC'
        e['bfres'], e['mainModel'] = r['bfres'], r['mainModel']
        for k in ('aabbMin', 'aabbMax', 'boundingForTraverse'):
            if k in r:
                e[k] = r[k]
        if 'instSize' in r and int(r['instSize']) > int(e['instSize']):
            e['instSize'] = oead.S32(int(r['instSize']))
        if HEAP_SCALE != 1.0 and 'instSize' in e:  # (2026-11-21: a tester's crash 2 was a new horse puppet's AS setup)
            e['instSize'] = oead.S32(int(int(e['instSize']) * HEAP_SCALE))
        e['traverseDist'] = oead.F32(99999.0)
        rows.append((zlib.crc32(name.encode()), e))
    rows.sort(key=lambda r: r[0])
    info['Hashes'] = oead.byml.Array([oead.U32(h) if h > 0x7FFFFFFF else oead.S32(h) for h, _ in rows])
    info['Actors'] = oead.byml.Array([a for _, a in rows])
    with open(os.path.join(OUT, 'Actor', 'ActorInfo.product.sbyml'), 'wb') as f:
        f.write(oead.yaz0.compress(oead.byml.to_binary(info, big_endian=False, version=2)))
    write_tables(player, OUT)
    # Runtime feature gate for overlays built before bow animations were added.
    with open(os.path.join(OUT, 'coop_bow_anim'), 'w'):
        pass
    shown = {k: v for k, v in sizes.items() if not k.startswith(('Dm_CoopW_', 'Dm_CoopA_', HORSE_PREFIX))}
    nw = sum(k.startswith('Dm_CoopW_') for k in sizes)
    print(f'{OUT}: {shown} + {nw} weapon/shield/bow puppets + {len(sizes) - len(shown) - nw - nh} armor puppets + '
          f'{nh} horse puppets, ActorInfo {len(info["Actors"])} actors')


if __name__ == '__main__':
    sys.exit(main())
