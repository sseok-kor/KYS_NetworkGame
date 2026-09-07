#pragma once
#include "../GameServer/Types/Defines.h"   // BYTE
#include "GameDefines.h"                   // ITEM_NAME_MAX

// 아이템 종류 대분류 - 장착 가능한 슬롯과 사용 가능 여부를 가른다.
enum class EItemType : BYTE
{
    EQUIP_WEAPON = 0,   // 무기 (장착 시 공격력 가산)
    EQUIP_ARMOR  = 1,   // 방어구 (장착 시 방어력 가산)
    CONSUME      = 2,   // 소모품 (사용 시 효과 적용 후 수량 감소)
    ETC          = 3,   // 기타 (재료/전리품 - 거래와 판매용)
    COUNT        = 4,   // 종류 개수 (범위 검증용, wire엔 안 실림)
};

// 공유 아이템 사전 - 서버(검증 + 효과 적용)와 클라(이름/모양 표시)가
//   같은 파일(Data/items.csv)을 부팅 시 적재하는 단일 진실원.
//   wire 엔 templateId 숫자만 실리고 이름/능력치는 양쪽이 이 표에서 조회한다.
//   드랍 확률은 서버 전용 Data/monsters.yml 의 drops 목록 소관 - 클라에 노출하지 않으려고 이 표엔 없다.
struct ItemDef
{
    int      templateId;            // 아이템 종류 번호 (0 초과, 표 안에서 유일)
    wchar_t  name[ITEM_NAME_MAX];   // 표시 이름 (한글 가능, CSV는 UTF-8 저장)
    EItemType itemType;             // 대분류 (장착/사용 판정)
    int      atkPower;              // 무기 장착 시 공격력 가산
    int      defPower;              // 방어구 장착 시 방어력 가산 (받는 피해 감산)
    int      hpRestore;             // 소모품 사용 시 HP 회복량
    int      stackMax;              // 한 슬롯 최대 수량 (장비 = 1)
};

// 부팅 1회: Data/items.csv 적재. 반환 = 적재한 아이템 수 (파일 없음/0행 = -1, 서버는 fail-fast).
int             LoadItemTable();

// 적재된 아이템 종류 수.
int             ItemTableCount();

// templateId 로 아이템 정의 조회. 없으면 nullptr - 참조 무결성 검증의 단일 관문
//   (인벤 로드 행 검증 / 드랍 표 검증 / 획득 경로 전부 이 함수를 거친다).
const ItemDef*  FindItemDef(int templateId);
