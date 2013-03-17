/* Orx - Portable Game Engine
 *
 * Copyright (c) 2008-2013 Orx-Project
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *    1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 *
 *    2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 *
 *    3. This notice may not be removed or altered from any source
 *    distribution.
 */

/**
 * @file orxCommand.c
 * @date 29/04/2012
 * @author iarwain@orx-project.org
 *
 */


#include "core/orxCommand.h"

#include "debug/orxDebug.h"
#include "debug/orxProfiler.h"
#include "core/orxConsole.h"
#include "core/orxEvent.h"
#include "memory/orxMemory.h"
#include "memory/orxBank.h"
#include "object/orxTimeLine.h"
#include "utils/orxString.h"
#include "utils/orxTree.h"

#ifdef __orxMSVC__

  #include "malloc.h"
  #pragma warning(disable : 4200)

#endif /* __orxMSVC__ */


/** Module flags
 */
#define orxCOMMAND_KU32_STATIC_FLAG_NONE              0x00000000                      /**< No flags */

#define orxCOMMAND_KU32_STATIC_FLAG_READY             0x00000001                      /**< Ready flag */
#define orxCOMMAND_KU32_STATIC_FLAG_PROCESSING_EVENT  0x10000000                      /** <Processing event flag */

#define orxCOMMAND_KU32_STATIC_MASK_ALL               0xFFFFFFFF                      /**< All mask */


/** Misc
 */
#define orxCOMMAND_KC_BLOCK_MARKER                    '"'                             /**< Block marker character */
#define orxCOMMAND_KC_PUSH_MARKER                     '>'                             /**< Push marker character */
#define orxCOMMAND_KC_POP_MARKER                      '<'                             /**< Pop marker character */
#define orxCOMMAND_KC_GUID_MARKER                     '^'                             /**< GUID marker character */


#define orxCOMMAND_KU32_TABLE_SIZE                    256
#define orxCOMMAND_KU32_BANK_SIZE                     128
#define orxCOMMAND_KU32_TRIE_BANK_SIZE                1024
#define orxCOMMAND_KU32_RESULT_BANK_SIZE              16
#define orxCOMMAND_KU32_RESULT_BUFFER_SIZE            256

#define orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE          4096
#define orxCOMMAND_KU32_PROTOTYPE_BUFFER_SIZE         512

#define orxCOMMAND_KZ_ERROR_VALUE                     "ERROR"
#define orxCOMMAND_KZ_STACK_ERROR_VALUE               "STACK_ERROR"


/***************************************************************************
 * Structure declaration                                                   *
 ***************************************************************************/

/** Command stack entry
 */
typedef struct __orxCOMMAND_STACK_ENTRY_t
{
  orxCOMMAND_VAR            stValue;                                                  /**< Value : 28 */

} orxCOMMAND_STACK_ENTRY;

/** Command structure
 */
typedef struct __orxCOMMAND_t
{
  orxSTRING                 zName;                                                    /**< Name : 4 */
  orxBOOL                   bIsAlias;                                                 /**> Is an alias? : 8 */

  union
  {
    struct
    {
      orxSTRING             zAliasedCommandName;                                      /**< Aliased command name : 12 */
      orxSTRING             zArgs;                                                    /**< Arguments : 16 */
    };

    struct
    {
      orxCOMMAND_FUNCTION   pfnFunction;                                              /**< Function : 12 */
      orxCOMMAND_VAR_DEF    stResult;                                                 /**< Result definition : 20 */
      orxU16                u16RequiredParamNumber;                                   /**< Required param number : 22 */
      orxU16                u16OptionalParamNumber;                                   /**< Optional param number : 24 */
      orxCOMMAND_VAR_DEF   *astParamList;                                             /**< Param list : 28 */
    };
  };

} orxCOMMAND;

/** Command trie node
 */
typedef struct __orxCOMMAND_TRIE_NODE_t
{
  orxTREE_NODE              stNode;
  orxCOMMAND               *pstCommand;
  orxU32                    u32CharacterCodePoint;

} orxCOMMAND_TRIE_NODE;

/** Static structure
 */
typedef struct __orxCOMMAND_STATIC_t
{
  orxBANK                  *pstBank;                                                  /**< Command bank */
  orxBANK                  *pstTrieBank;                                              /**< Command trie bank */
  orxTREE                   stCommandTrie;                                            /**< Command trie */
  orxBANK                  *pstResultBank;                                            /**< Command result bank */
  orxCHAR                   acEvaluateBuffer[orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE];   /**< Evaluate buffer */
  orxCHAR                   acPrototypeBuffer[orxCOMMAND_KU32_PROTOTYPE_BUFFER_SIZE]; /**< Prototype buffer */
  orxCHAR                   acResultBuffer[orxCOMMAND_KU32_RESULT_BUFFER_SIZE];       /**< Result buffer */
  orxU32                    u32Flags;                                                 /**< Control flags */

} orxCOMMAND_STATIC;


/***************************************************************************
 * Static variables                                                        *
 ***************************************************************************/

/** Static data
 */
static orxCOMMAND_STATIC sstCommand;


/***************************************************************************
 * Private functions                                                       *
 ***************************************************************************/

/** Parses numerical commands
 */
static orxINLINE orxSTATUS orxCommand_ParseNumericalArguments(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_astOperandList)
{
  orxU32    i;
  orxSTATUS eResult = orxSTATUS_SUCCESS;

  /* For all arguments */
  for(i = 0; i < _u32ArgNumber; i++)
  {
    /* Gets vector operand */
    if(orxString_ToVector(_astArgList[i].zValue, &(_astOperandList[i].vValue), orxNULL) != orxSTATUS_FAILURE)
    {
      /* Updates its type */
      _astOperandList[i].eType = orxCOMMAND_VAR_TYPE_VECTOR;
    }
    else
    {
      /* Hexadecimal, binary or octal?? */
      if((_astArgList[i].zValue[0] != orxCHAR_EOL)
      && (_astArgList[i].zValue[0] == '0')
      && (_astArgList[i].zValue[1] != orxCHAR_EOL)
      && (((_astArgList[i].zValue[1] | 0x20) == 'x')
       || ((_astArgList[i].zValue[1] | 0x20) == 'b')
       || ((_astArgList[i].zValue[1] >= '0')
        && (_astArgList[i].zValue[1] <= '9'))))
      {
        /* Gets U64 operand */
        if(orxString_ToU64(_astArgList[i].zValue, &(_astOperandList[i].u64Value), orxNULL) != orxSTATUS_FAILURE)
        {
          /* Gets its float value */
          _astOperandList[i].fValue = orxU2F(_astOperandList[i].u64Value);

          /* Updates its type */
          _astOperandList[i].eType = orxCOMMAND_VAR_TYPE_FLOAT;
        }
        else
        {
          /* Updates result */
          eResult = orxSTATUS_FAILURE;

          break;
        }
      }
      else
      {
        /* Gets float operand */
        if(orxString_ToFloat(_astArgList[i].zValue, &(_astOperandList[i].fValue), orxNULL) != orxSTATUS_FAILURE)
        {
          /* Updates its type */
          _astOperandList[i].eType = orxCOMMAND_VAR_TYPE_FLOAT;
        }
        else
        {
          /* Updates result */
          eResult = orxSTATUS_FAILURE;

          break;
        }
      }
    }
  }

  /* Done! */
  return eResult;
}

/** Gets literal name of a command var type
 */
static orxINLINE const orxSTRING orxCommand_GetTypeString(orxCOMMAND_VAR_TYPE _eType)
{
  const orxSTRING zResult;

#define orxCOMMAND_DECLARE_TYPE_NAME(TYPE) case orxCOMMAND_VAR_TYPE_##TYPE: zResult = "orx"#TYPE; break

  /* Depending on type */
  switch(_eType)
  {
    orxCOMMAND_DECLARE_TYPE_NAME(STRING);
    orxCOMMAND_DECLARE_TYPE_NAME(FLOAT);
    orxCOMMAND_DECLARE_TYPE_NAME(S32);
    orxCOMMAND_DECLARE_TYPE_NAME(U32);
    orxCOMMAND_DECLARE_TYPE_NAME(S64);
    orxCOMMAND_DECLARE_TYPE_NAME(U64);
    orxCOMMAND_DECLARE_TYPE_NAME(BOOL);
    orxCOMMAND_DECLARE_TYPE_NAME(VECTOR);

    default:
    {
      /* Logs message */
      orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "No name defined for command var type #%d.", _eType);

      /* Updates result */
      zResult = orxSTRING_EMPTY;
    }
  }

  /* Done! */
  return zResult;
}

/** Runs a command
 */
static orxINLINE orxCOMMAND_VAR *orxCommand_Run(const orxCOMMAND *_pstCommand, orxBOOL _bCheckArgList, orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR *pstResult = orxNULL;

  /* Valid number of arguments? */
  if((_bCheckArgList == orxFALSE)
  || ((_u32ArgNumber >= (orxU32)_pstCommand->u16RequiredParamNumber)
   && (_u32ArgNumber <= (orxU32)_pstCommand->u16RequiredParamNumber + (orxU32)_pstCommand->u16OptionalParamNumber)))
  {
    orxU32 i;

    /* Checks ? */
    if(_bCheckArgList != orxFALSE)
    {
      /* For all arguments */
      for(i = 0; i < _u32ArgNumber; i++)
      {
        /* Invalid? */
        if(_astArgList[i].eType != _pstCommand->astParamList[i].eType)
        {
          /* Logs message */
          orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't execute command [%s]: invalid type for argument #%d (%s).", _pstCommand->zName, i + 1, _pstCommand->astParamList[i].zName);

          /* Stops */
          break;
        }
      }
    }
    else
    {
      /* Validates it */
      i = _u32ArgNumber;
    }

    /* Valid? */
    if(i == _u32ArgNumber)
    {
      /* Inits result */
      _pstResult->eType = _pstCommand->stResult.eType;

      /* Runs command */
      _pstCommand->pfnFunction(_u32ArgNumber, _astArgList, _pstResult);

      /* Updates result */
      pstResult = _pstResult;
    }
  }

  /* Done! */
  return pstResult;
}

static orxINLINE orxCOMMAND_TRIE_NODE *orxCommand_FindTrieNode(const orxSTRING _zName, orxBOOL _bInsert)
{
  const orxSTRING       zName;
  orxU32                u32CharacterCodePoint;
  orxCOMMAND_TRIE_NODE *pstNode, *pstResult = orxNULL;

  /* Gets trie root */
  pstNode = (orxCOMMAND_TRIE_NODE *)orxTree_GetRoot(&(sstCommand.stCommandTrie));

  /* For all characters */
  for(u32CharacterCodePoint = orxString_GetFirstCharacterCodePoint(_zName, &zName);
      u32CharacterCodePoint != orxCHAR_NULL;
      u32CharacterCodePoint = orxString_GetFirstCharacterCodePoint(zName, &zName))
  {
    orxCOMMAND_TRIE_NODE *pstChild, *pstPrevious;

    /* Is an upper case ASCII character? */
    if((orxString_IsCharacterASCII(u32CharacterCodePoint) != orxFALSE)
    && (u32CharacterCodePoint >= 'A')
    && (u32CharacterCodePoint <= 'Z'))
    {
      /* Gets its lower case version */
      u32CharacterCodePoint |= 0x20;
    }

    /* Finds the matching place in children */
    for(pstPrevious = orxNULL, pstChild = (orxCOMMAND_TRIE_NODE *)orxTree_GetChild(&(pstNode->stNode));
        (pstChild != orxNULL) && (pstChild->u32CharacterCodePoint < u32CharacterCodePoint);
        pstPrevious = pstChild, pstChild = (orxCOMMAND_TRIE_NODE *)orxTree_GetSibling(&(pstChild->stNode)));

    /* Not found? */
    if((pstChild == orxNULL)
    || (pstChild->u32CharacterCodePoint != u32CharacterCodePoint))
    {
      /* Insertion allowed? */
      if(_bInsert != orxFALSE)
      {
        /* Creates new trie node */
        pstChild = (orxCOMMAND_TRIE_NODE *)orxBank_Allocate(sstCommand.pstTrieBank);

        /* Checks */
        orxASSERT(pstChild != orxNULL);

        /* Inits it */
        orxMemory_Zero(pstChild, sizeof(orxCOMMAND_TRIE_NODE));

        /* Has previous? */
        if(pstPrevious != orxNULL)
        {
          /* Inserts it as sibling */
          orxTree_AddSibling(&(pstPrevious->stNode), &(pstChild->stNode));
        }
        else
        {
          /* Inserts it as child */
          orxTree_AddChild(&(pstNode->stNode), &(pstChild->stNode));
        }

        /* Stores character code point */
        pstChild->u32CharacterCodePoint = u32CharacterCodePoint;
      }
      else
      {
        /* Stops search */
        break;
      }
    }

    /* End of name? */
    if(*zName == orxCHAR_NULL)
    {
      /* Updates result */
      pstResult = pstChild;

      break;
    }
    else
    {
      /* Stores next node */
      pstNode = pstChild;
    }
  }

  /* Done! */
  return pstResult;
}

static orxINLINE orxCOMMAND *orxCommand_FindNoAlias(const orxSTRING _zCommand)
{
  orxCOMMAND_TRIE_NODE *pstNode;
  orxCOMMAND           *pstResult;

  /* Finds right command */
  for(pstNode = orxCommand_FindTrieNode(_zCommand, orxFALSE);
      (pstNode != orxNULL) && (pstNode->pstCommand != orxNULL) && (pstNode->pstCommand->bIsAlias != orxFALSE);
      pstNode = orxCommand_FindTrieNode(pstNode->pstCommand->zAliasedCommandName, orxFALSE));

  /* Updates result */
  pstResult = (pstNode != orxNULL) ? pstNode->pstCommand : orxNULL;

  /* Done! */
  return pstResult;
}

static orxINLINE void orxCommand_InsertInTrie(orxCOMMAND *_pstCommand)
{
  orxCOMMAND_TRIE_NODE *pstNode;

  /* Gets command trie node */
  pstNode = orxCommand_FindTrieNode(_pstCommand->zName, orxTRUE);

  /* Checks */
  orxASSERT(pstNode != orxNULL);
  orxASSERT(pstNode->pstCommand == orxNULL);

  /* Inserts command */
  pstNode->pstCommand = _pstCommand;

  /* Done! */
  return;
}

static orxINLINE void orxCommand_RemoveFromTrie(orxCOMMAND *_pstCommand)
{
  orxCOMMAND_TRIE_NODE *pstNode;

  /* Finds command trie node */
  pstNode = orxCommand_FindTrieNode(_pstCommand->zName, orxFALSE);

  /* Checks */
  orxASSERT(pstNode != orxNULL);
  orxASSERT(pstNode->pstCommand == _pstCommand);

  /* Removes command */
  pstNode->pstCommand = orxNULL;

  /* Done! */
  return;
}

static orxINLINE const orxCOMMAND *orxCommand_FindNext(const orxCOMMAND_TRIE_NODE *_pstNode, orxCOMMAND_TRIE_NODE **_ppstPreviousNode)
{
  const orxCOMMAND *pstResult = orxNULL;

  /* Valid node? */
  if(_pstNode != orxNULL)
  {
    /* Has reached previous command node and found a new command? */
    if((*_ppstPreviousNode == orxNULL)
    && (_pstNode->pstCommand != orxNULL))
    {
      /* Updates result */
      pstResult = _pstNode->pstCommand;
    }
    else
    {
      /* Passed previous command node? */
      if(*_ppstPreviousNode == _pstNode)
      {
        /* Resets previous command node value */
        *_ppstPreviousNode = orxNULL;
      }

      /* Finds next command from child */
      pstResult = orxCommand_FindNext((orxCOMMAND_TRIE_NODE *)orxTree_GetChild(&(_pstNode->stNode)), _ppstPreviousNode);

      /* No command found? */
      if(pstResult == orxNULL)
      {
        /* Finds next command from sibling */
        pstResult = orxCommand_FindNext((orxCOMMAND_TRIE_NODE *)orxTree_GetSibling(&(_pstNode->stNode)), _ppstPreviousNode);
      }
    }
  }

  /* Done! */
  return pstResult;
}

static orxCOMMAND_VAR *orxFASTCALL orxCommand_Process(const orxSTRING _zCommandLine, const orxU64 _u64GUID, orxCOMMAND_VAR *_pstResult)
{
  orxSTRING       zCommand;
  orxCOMMAND_VAR *pstResult = orxNULL;

  /* Profiles */
  orxPROFILER_PUSH_MARKER("orxCommand_Process");

  /* Gets start of command */
  zCommand = (orxCHAR *)orxString_SkipWhiteSpaces(_zCommandLine);

  /* Valid? */
  if(zCommand != orxSTRING_EMPTY)
  {
    orxU32      u32PushCounter;
    orxCHAR    *pcCommandEnd, cBackupChar;
    orxCOMMAND *pstCommand;
    orxCHAR     acGUID[20];

    /* For all push markers / spaces */
    for(u32PushCounter = 0; (*zCommand == orxCOMMAND_KC_PUSH_MARKER) || (*zCommand == ' ') || (*zCommand == '\t'); zCommand++)
    {
      /* Is a push marker? */
      if(*zCommand == orxCOMMAND_KC_PUSH_MARKER)
      {
        /* Updates push counter */
        u32PushCounter++;
      }
    }

    /* Finds end of command */
    for(pcCommandEnd = zCommand + 1; (*pcCommandEnd != orxCHAR_NULL) && (*pcCommandEnd != ' ') && (*pcCommandEnd != '\t') && (*pcCommandEnd != orxCHAR_CR) && (*pcCommandEnd != orxCHAR_LF); pcCommandEnd++);

    /* Ends command */
    cBackupChar               = *pcCommandEnd;
    *(orxCHAR *)pcCommandEnd  = orxCHAR_NULL;

    /* Gets it */
    pstCommand = orxCommand_FindNoAlias(zCommand);

    /* Found? */
    if(pstCommand != orxNULL)
    {
#define orxCOMMAND_KU32_ALIAS_MAX_DEPTH             32
      orxSTATUS             eStatus;
      orxS32                s32GUIDLength, s32BufferCounter = 0, i;
      orxBOOL               bInBlock = orxFALSE;
      orxCOMMAND_TRIE_NODE *pstCommandNode;
      const orxCHAR        *pcSrc;
      orxCHAR              *pcDst;
      const orxSTRING       zArg;
      const orxSTRING       azBufferList[orxCOMMAND_KU32_ALIAS_MAX_DEPTH];
      orxU32                u32ArgNumber, u32ParamNumber = (orxU32)pstCommand->u16RequiredParamNumber + (orxU32)pstCommand->u16OptionalParamNumber;

#ifdef __orxMSVC__

      orxCOMMAND_VAR *astArgList = (orxCOMMAND_VAR *)alloca(u32ParamNumber * sizeof(orxCOMMAND_VAR));

#else /* __orxMSVC__ */

      orxCOMMAND_VAR astArgList[u32ParamNumber];

#endif /* __orxMSVC__ */

      /* Gets owner's GUID */
      acGUID[19]    = orxCHAR_NULL;
      s32GUIDLength = orxString_NPrint(acGUID, 19, "0x%016llX", _u64GUID);

      /* Adds input to the buffer list */
      azBufferList[s32BufferCounter++] = pcCommandEnd + 1;

      /* For all alias nodes */
      for(pstCommandNode = orxCommand_FindTrieNode(zCommand, orxFALSE);
          (pstCommandNode->pstCommand->bIsAlias != orxFALSE) && (s32BufferCounter < orxCOMMAND_KU32_ALIAS_MAX_DEPTH);
          pstCommandNode = orxCommand_FindTrieNode(pstCommandNode->pstCommand->zAliasedCommandName, orxFALSE))
      {
        /* Has args? */
        if(pstCommandNode->pstCommand->zArgs != orxNULL)
        {
          /* Adds it to the buffer list */
          azBufferList[s32BufferCounter++] = pstCommandNode->pstCommand->zArgs;
        }
      }

      /* Restores command end */
      *(orxCHAR *)pcCommandEnd = cBackupChar;

      /* For all stacked buffers */
      for(i = s32BufferCounter - 1, pcDst = sstCommand.acEvaluateBuffer; i >= 0; i--)
      {
        /* Has room for next buffer? */
        if((i != s32BufferCounter - 1) && (*azBufferList[i] != orxCHAR_NULL) && (pcDst - sstCommand.acEvaluateBuffer < orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE - 2))
        {
          /* Inserts space */
          *pcDst++ = ' ';
        }

        /* For all characters */
        for(pcSrc = azBufferList[i]; (*pcSrc != orxCHAR_NULL) && (pcDst - sstCommand.acEvaluateBuffer < orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE - 2); pcSrc++)
        {
          /* Depending on character */
          switch(*pcSrc)
          {
            case orxCOMMAND_KC_GUID_MARKER:
            {
              /* Replaces it with GUID */
              orxString_NCopy(pcDst, acGUID, orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE - 1 - (pcDst - sstCommand.acEvaluateBuffer));

              /* Updates pointer */
              pcDst += s32GUIDLength;

              break;
            }

            case orxCOMMAND_KC_POP_MARKER:
            {
              /* Valid? */
              if(orxBank_GetCounter(sstCommand.pstResultBank) > 0)
              {
                orxCOMMAND_STACK_ENTRY *pstEntry;
                orxCHAR                 acValue[64];
                orxBOOL                 bUseStringMarker = orxFALSE;
                const orxSTRING         zValue = acValue;
                const orxCHAR          *pc;

                /* Gets last stack entry */
                pstEntry = (orxCOMMAND_STACK_ENTRY *)orxBank_GetAtIndex(sstCommand.pstResultBank, orxBank_GetCounter(sstCommand.pstResultBank) - 1);

                /* Inits value */
                acValue[63] = orxCHAR_NULL;

                /* Depending on type */
                switch(pstEntry->stValue.eType)
                {
                  default:
                  case orxCOMMAND_VAR_TYPE_STRING:
                  {
                    /* Updates pointer */
                    zValue = pstEntry->stValue.zValue;

                    /* Is not in block? */
                    if(bInBlock == orxFALSE)
                    {
                      /* For all characters */
                      for(pc = zValue; *pc != orxCHAR_NULL; pc++)
                      {
                        /* Is a white space? */
                        if((*pc == ' ') || (*pc == '\t'))
                        {
                          /* Has room? */
                          if(pcDst - sstCommand.acEvaluateBuffer < orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE - 1)
                          {
                            /* Adds block marker */
                            *pcDst++ = orxCOMMAND_KC_BLOCK_MARKER;

                            /* Updates string marker status */
                            bUseStringMarker = orxTRUE;
                          }

                          break;
                        }
                      }
                    }

                    break;
                  }

                  case orxCOMMAND_VAR_TYPE_FLOAT:
                  {
                    /* Stores it */
                    orxString_NPrint(acValue, 63, "%g", pstEntry->stValue.fValue);

                    break;
                  }

                  case orxCOMMAND_VAR_TYPE_S32:
                  {
                    /* Stores it */
                    orxString_NPrint(acValue, 63, "%d", pstEntry->stValue.s32Value);

                    break;
                  }

                  case orxCOMMAND_VAR_TYPE_U32:
                  {
                    /* Stores it */
                    orxString_NPrint(acValue, 63, "%u", pstEntry->stValue.u32Value);

                    break;
                  }

                  case orxCOMMAND_VAR_TYPE_S64:
                  {
                    /* Stores it */
                    orxString_NPrint(acValue, 63, "%lld", pstEntry->stValue.s64Value);

                    break;
                  }

                  case orxCOMMAND_VAR_TYPE_U64:
                  {
                    /* Stores it */
                    orxString_NPrint(acValue, 63, "0x%016llX", pstEntry->stValue.u64Value);

                    break;
                  }

                  case orxCOMMAND_VAR_TYPE_BOOL:
                  {
                    /* Stores it */
                    orxString_NPrint(acValue, 63, "%s", (pstEntry->stValue.bValue == orxFALSE) ? orxSTRING_FALSE : orxSTRING_TRUE);

                    break;
                  }

                  case orxCOMMAND_VAR_TYPE_VECTOR:
                  {
                    /* Gets literal value */
                    orxString_NPrint(acValue, 63, "%c%g%c%g%c%g%c", orxSTRING_KC_VECTOR_START, pstEntry->stValue.vValue.fX, orxSTRING_KC_VECTOR_SEPARATOR, pstEntry->stValue.vValue.fY, orxSTRING_KC_VECTOR_SEPARATOR, pstEntry->stValue.vValue.fZ, orxSTRING_KC_VECTOR_END);

                    break;
                  }
                }

                /* Replaces marker with stacked value */
                orxString_NCopy(pcDst, zValue, orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE - 1 - (pcDst - sstCommand.acEvaluateBuffer));

                /* Updates pointers */
                pcDst += orxString_GetLength(zValue);

                /* Used a string marker? */
                if(bUseStringMarker != orxFALSE)
                {
                  /* Has room? */
                  if(pcDst - sstCommand.acEvaluateBuffer < orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE - 1)
                  {
                    *pcDst++ = orxCOMMAND_KC_BLOCK_MARKER;
                  }

                  /* Deletes it */
                  orxString_Delete((orxCHAR *)pstEntry->stValue.zValue);
                }

                /* Deletes stack entry */
                orxBank_Free(sstCommand.pstResultBank, pstEntry);
              }
              else
              {
                /* Logs message */
                orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't pop stacked argument for command line [%s]: stack is empty.", _zCommandLine);

                /* Replaces marker with stack error */
                orxString_NCopy(pcDst, orxCOMMAND_KZ_STACK_ERROR_VALUE, orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE - 1 - (pcDst - sstCommand.acEvaluateBuffer));

                /* Updates pointers */
                pcDst += orxString_GetLength(orxCOMMAND_KZ_ERROR_VALUE);
              }

              break;
            }

            case orxCOMMAND_KC_BLOCK_MARKER:
            {
              /* Toggles block status */
              bInBlock = !bInBlock;

              /* Falls through */
            }

            default:
            {
              /* Copies it */
              *pcDst++ = *pcSrc;

              break;
            }
          }
        }
      }

      /* Copies end of string */
      *pcDst = orxCHAR_NULL;

      /* For all characters in the buffer */
      for(pcSrc = sstCommand.acEvaluateBuffer, eStatus = orxSTATUS_SUCCESS, zArg = orxSTRING_EMPTY, u32ArgNumber = 0;
          (u32ArgNumber < u32ParamNumber) && (pcSrc - sstCommand.acEvaluateBuffer < orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE) && (*pcSrc != orxCHAR_NULL);
          pcSrc++, u32ArgNumber++)
      {
        /* Skips all whitespaces */
        pcSrc = orxString_SkipWhiteSpaces(pcSrc);

        /* Valid? */
        if(*pcSrc != orxCHAR_NULL)
        {
          orxBOOL bInBlock = orxFALSE;

          /* Gets arg's beginning */
          zArg = pcSrc;

          /* Is a block marker? */
          if(*pcSrc == orxCOMMAND_KC_BLOCK_MARKER)
          {
            /* Updates arg pointer */
            zArg++;
            pcSrc++;

            /* Updates block status */
            bInBlock = orxTRUE;
          }

          /* Stores its type */
          astArgList[u32ArgNumber].eType = pstCommand->astParamList[u32ArgNumber].eType;

          /* Depending on its type */
          switch(pstCommand->astParamList[u32ArgNumber].eType)
          {
            default:
            case orxCOMMAND_VAR_TYPE_STRING:
            {
              /* Finds end of argument */
              for(; *pcSrc != orxCHAR_NULL; pcSrc++)
              {
                /* Is a block marker? */
                if(*pcSrc == orxCOMMAND_KC_BLOCK_MARKER)
                {
                  orxCHAR *pcTemp;

                  /* Erases it */
                  for(pcTemp = (orxCHAR *)pcSrc; *pcTemp != orxNULL; pcTemp++)
                  {
                    *pcTemp = *(pcTemp + 1);
                  }

                  /* Updates block status */
                  bInBlock = !bInBlock;

                  /* Double marker? */
                  if(*pcSrc == orxCOMMAND_KC_BLOCK_MARKER)
                  {
                    /* Updates block status */
                    bInBlock = !bInBlock;
                  }
                  else
                  {
                    /* Handles current character in new mode */
                    pcSrc--;
                  }
                  continue;
                }

                /* Not in block? */
                if(bInBlock == orxFALSE)
                {
                  /* End of string? */
                  if((*pcSrc == ' ') || (*pcSrc == '\t'))
                  {
                    /* Stops */
                    break;
                  }
                }
              }

              /* Stores its value */
              astArgList[u32ArgNumber].zValue = zArg;

              break;
            }

            case orxCOMMAND_VAR_TYPE_FLOAT:
            {
              /* Gets its value */
              eStatus = orxString_ToFloat(zArg, &(astArgList[u32ArgNumber].fValue), &pcSrc);

              break;
            }

            case orxCOMMAND_VAR_TYPE_S32:
            {
              /* Gets its value */
              eStatus = orxString_ToS32(zArg, &(astArgList[u32ArgNumber].s32Value), &pcSrc);

              break;
            }

            case orxCOMMAND_VAR_TYPE_U32:
            {
              /* Gets its value */
              eStatus = orxString_ToU32(zArg, &(astArgList[u32ArgNumber].u32Value), &pcSrc);

              break;
            }

            case orxCOMMAND_VAR_TYPE_S64:
            {
              /* Gets its value */
              eStatus = orxString_ToS64(zArg, &(astArgList[u32ArgNumber].s64Value), &pcSrc);

              break;
            }

            case orxCOMMAND_VAR_TYPE_U64:
            {
              /* Gets its value */
              eStatus = orxString_ToU64(zArg, &(astArgList[u32ArgNumber].u64Value), &pcSrc);

              break;
            }

            case orxCOMMAND_VAR_TYPE_BOOL:
            {
              /* Gets its value */
              eStatus = orxString_ToBool(zArg, &(astArgList[u32ArgNumber].bValue), &pcSrc);

              break;
            }

            case orxCOMMAND_VAR_TYPE_VECTOR:
            {
              /* Gets its value */
              eStatus = orxString_ToVector(zArg, &(astArgList[u32ArgNumber].vValue), &pcSrc);

              break;
            }
          }

          /* Interrupted? */
          if((eStatus == orxSTATUS_FAILURE) || (*pcSrc == orxCHAR_NULL))
          {
            /* Updates argument counter */
            u32ArgNumber++;

            /* Stops processing */
            break;
          }
          else
          {
            /* Ends current argument */
            *(orxCHAR *)pcSrc = orxCHAR_NULL;
          }
        }
      }

      /* Error? */
      if((eStatus == orxSTATUS_FAILURE) || (u32ArgNumber < (orxU32)pstCommand->u16RequiredParamNumber))
      {
        /* Incorrect parameter? */
        if(eStatus == orxSTATUS_FAILURE)
        {
          /* Logs message */
          orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't evaluate command line [%s], invalid argument #%d.", _zCommandLine, u32ArgNumber);
        }
        else
        {
          /* Logs message */
          orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't evaluate command line [%s], expected %d[+%d] arguments, found %d.", _zCommandLine, (orxU32)pstCommand->u16RequiredParamNumber, (orxU32)pstCommand->u16OptionalParamNumber, u32ArgNumber);
        }
      }
      else
      {
        /* Runs it */
        pstResult = orxCommand_Run(pstCommand, orxFALSE, u32ArgNumber, astArgList, _pstResult);
      }
    }
    else
    {
      /* Restores command end */
      *(orxCHAR *)pcCommandEnd = cBackupChar;

      /* Not processing event? */
      if(!orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_PROCESSING_EVENT))
      {
        /* Logs message */
        orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't evaluate command line [%s], invalid command.", _zCommandLine);
      }
    }

    /* Failure? */
    if(pstResult == orxNULL)
    {
      /* Stores error */
      _pstResult->eType   = orxCOMMAND_VAR_TYPE_STRING;
      _pstResult->zValue  = orxCOMMAND_KZ_ERROR_VALUE;
    }

    /* For all requested pushes */
    while(u32PushCounter > 0)
    {
      orxCOMMAND_STACK_ENTRY *pstEntry;

      /* Allocates stack entry */
      pstEntry = (orxCOMMAND_STACK_ENTRY *)orxBank_Allocate(sstCommand.pstResultBank);

      /* Checks */
      orxASSERT(pstEntry != orxNULL);

      /* Is a string value? */
      if(_pstResult->eType == orxCOMMAND_VAR_TYPE_STRING)
      {
        /* Duplicates it */
        pstEntry->stValue.eType = orxCOMMAND_VAR_TYPE_STRING;
        pstEntry->stValue.zValue = orxString_Duplicate(_pstResult->zValue);
      }
      else
      {
        /* Stores value */
        orxMemory_Copy(&(pstEntry->stValue), _pstResult, sizeof(orxCOMMAND_STACK_ENTRY));
      }

      /* Updates push counter */
      u32PushCounter--;
    }
  }
  else
  {
    /* Logs message */
    orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't evaluate command line [%s]: [%s] is not a registered command.", _zCommandLine, zCommand);
  }

  /* Profiles */
  orxPROFILER_POP_MARKER();

  /* Done! */
  return pstResult;
}

/** Event handler
 * @param[in]   _pstEvent                     Sent event
 * @return      orxSTATUS_SUCCESS if handled / orxSTATUS_FAILURE otherwise
 */
static orxSTATUS orxFASTCALL orxCommand_EventHandler(const orxEVENT *_pstEvent)
{
  orxSTATUS eResult = orxSTATUS_SUCCESS;

  /* Checks */
  orxASSERT(_pstEvent->eType == orxEVENT_TYPE_TIMELINE);

  /* Depending on event ID */
  switch(_pstEvent->eID)
  {
    /* Trigger */
    case orxTIMELINE_EVENT_TRIGGER:
    {
      orxCOMMAND_VAR              stResult;
      orxTIMELINE_EVENT_PAYLOAD  *pstPayload;

      /* Gets payload */
      pstPayload = (orxTIMELINE_EVENT_PAYLOAD *)_pstEvent->pstPayload;

      /* Updates internal status */
      orxFLAG_SET(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_PROCESSING_EVENT, orxCOMMAND_KU32_STATIC_FLAG_NONE);

      /* Processes command */
      orxCommand_Process(pstPayload->zEvent, orxStructure_GetGUID(orxSTRUCTURE(_pstEvent->hSender)), &stResult);

      /* Updates internal status */
      orxFLAG_SET(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_NONE, orxCOMMAND_KU32_STATIC_FLAG_PROCESSING_EVENT);

      break;
    }

    default:
    {
      break;
    }
  }

  /* Done! */
  return eResult;
}

/** Command: Help
 */
void orxFASTCALL orxCommand_CommandHelp(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* No argument? */
  if(_u32ArgNumber == 0)
  {
    /* Updates result */
    _pstResult->zValue = "Usage: Command.Help <Command> to get the prototype of a command.";
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxCommand_GetPrototype(_astArgList[0].zValue);
  }

  /* Done! */
  return;
}

/** Command: ListCommands
 */
void orxFASTCALL orxCommand_CommandListCommands(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  const orxSTRING zPrefix;
  const orxSTRING zCommand;
  orxU32          u32Counter;

  /* Gets prefix */
  zPrefix = (_u32ArgNumber > 0) ? _astArgList[0].zValue : orxNULL;

  /* For all commands */
  for(zCommand = orxNULL, zCommand = orxCommand_GetNext(zPrefix, zCommand, orxNULL), u32Counter = 0;
      zCommand != orxNULL;
      zCommand = orxCommand_GetNext(zPrefix, zCommand, orxNULL))
  {
    /* Is a command? */
    if(orxCommand_IsAlias(zCommand) == orxFALSE)
    {
      /* Logs it */
      orxConsole_Log(zCommand);

      /* Updates counter */
      u32Counter++;
    }
  }

  /* Updates result */
  _pstResult->u32Value = u32Counter;

  /* Done! */
  return;
}

/* Command: AddAlias */
void orxFASTCALL orxCommand_CommandAddAlias(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  const orxSTRING zArgs;

  /* Has arguments? */
  if(_u32ArgNumber > 2)
  {
    /* Uses them */
    zArgs = _astArgList[2].zValue;
  }
  else
  {
    /* No args */
     zArgs = orxNULL;
  }

  /* Adds alias */
  if(orxCommand_AddAlias(_astArgList[0].zValue, _astArgList[1].zValue, zArgs) != orxSTATUS_FAILURE)
  {
    /* Updates result */
    _pstResult->zValue = _astArgList[0].zValue;
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}
/* Command: RemoveAlias */
void orxFASTCALL orxCommand_CommandRemoveAlias(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* Removes alias */
  if(orxCommand_RemoveAlias(_astArgList[0].zValue) != orxSTATUS_FAILURE)
  {
    /* Updates result */
    _pstResult->zValue = _astArgList[0].zValue;
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/** Command: ListAliases
 */
void orxFASTCALL orxCommand_CommandListAliases(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  const orxSTRING zPrefix;
  const orxSTRING zAlias;
  orxU32          u32Counter;

  /* Gets prefix */
  zPrefix = (_u32ArgNumber > 0) ? _astArgList[0].zValue : orxNULL;

  /* For all commands */
  for(zAlias = orxNULL, zAlias = orxCommand_GetNext(zPrefix, zAlias, orxNULL), u32Counter = 0;
      zAlias != orxNULL;
      zAlias = orxCommand_GetNext(zPrefix, zAlias, orxNULL))
  {
    /* Is an alias? */
    if(orxCommand_IsAlias(zAlias) != orxFALSE)
    {
      orxCOMMAND_TRIE_NODE *pstAliasNode, *pstCommandNode;
      orxCHAR               acBuffer[256];

      /* Finds alias node */
      pstAliasNode = orxCommand_FindTrieNode(zAlias, orxFALSE);

      /* Checks */
      orxASSERT((pstAliasNode != orxNULL) && (pstAliasNode->pstCommand != orxNULL));

      /* Finds aliased node */
      pstCommandNode = orxCommand_FindTrieNode(pstAliasNode->pstCommand->zAliasedCommandName, orxFALSE);

      /* Valid? */
      if((pstCommandNode != orxNULL) && (pstCommandNode->pstCommand != orxNULL))
      {
        /* Has args? */
        if(pstAliasNode->pstCommand->zArgs != orxNULL)
        {
          /* Writes log */
          orxString_NPrint(acBuffer, 255, "%s -> %s +<%s> [%s]", zAlias, pstCommandNode->pstCommand->zName, pstAliasNode->pstCommand->zArgs, (pstCommandNode->pstCommand->bIsAlias != orxFALSE) ? "ALIAS" : "COMMAND");
          acBuffer[255] = orxCHAR_NULL;
        }
        else
        {
          /* Writes log */
          orxString_NPrint(acBuffer, 255, "%s -> %s [%s]", zAlias, pstCommandNode->pstCommand->zName, (pstCommandNode->pstCommand->bIsAlias != orxFALSE) ? "ALIAS" : "COMMAND");
          acBuffer[255] = orxCHAR_NULL;
        }
      }
      else
      {
        /* Writes log */
        orxString_NPrint(acBuffer, 255, "%s -> %s [UNBOUND]", zAlias, pstAliasNode->pstCommand->zAliasedCommandName);
        acBuffer[255] = orxCHAR_NULL;
      }

      /* Logs it */
      orxConsole_Log(acBuffer);

      /* Updates counter */
      u32Counter++;
    }
  }

  /* Updates result */
  _pstResult->u32Value = u32Counter;

  /* Done! */
  return;
}

/* Command: Evaluate */
void orxFASTCALL orxCommand_CommandEvaluate(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* Disables marker operations */
  orxProfiler_EnableMarkerOperations(orxFALSE);

  /* Evaluates command */
  orxCommand_Evaluate(_astArgList[0].zValue, _pstResult);

  /* Re-enables marker operations */
  orxProfiler_EnableMarkerOperations(orxTRUE);

  /* Done! */
  return;
}

/* Command: EvaluateIf  */
void orxFASTCALL orxCommand_CommandEvaluateIf(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxBOOL bTest;

  /* Disables marker operations */
  orxProfiler_EnableMarkerOperations(orxFALSE);

  /* Is true? */
  if((orxString_ToBool(_astArgList[0].zValue, &bTest, orxNULL) != orxSTATUS_FAILURE) && (bTest != orxFALSE))
  {
    /* Evaluates first command */
    orxCommand_Evaluate(_astArgList[1].zValue, _pstResult);
  }
  else
  {
    /* Has an alternate command? */
    if(_u32ArgNumber > 2)
    {
      /* Evaluates it */
      orxCommand_Evaluate(_astArgList[2].zValue, _pstResult);
    }
    else
    {
      /* Updates result */
      _pstResult->zValue = orxSTRING_EMPTY;
    }
  }

  /* Re-enables marker operations */
  orxProfiler_EnableMarkerOperations(orxTRUE);

  /* Done! */
  return;
}

/* Command: If */
void orxFASTCALL orxCommand_CommandIf(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxBOOL bTest;

  /* Is true? */
  if((orxString_ToBool(_astArgList[0].zValue, &bTest, orxNULL) != orxSTATUS_FAILURE) && (bTest != orxFALSE))
  {
    /* Updates result */
    _pstResult->zValue = _astArgList[1].zValue;
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = (_u32ArgNumber > 2) ? _astArgList[2].zValue : orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/* Command: Not */
void orxFASTCALL orxCommand_CommandNot(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* Updates result */
  _pstResult->bValue = !_astArgList[0].bValue;

  /* Done! */
  return;
}

/* Command: And */
void orxFASTCALL orxCommand_CommandAnd(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* Updates result */
  _pstResult->bValue = _astArgList[0].bValue && _astArgList[1].bValue;

  /* Done! */
  return;
}

/* Command: Or */
void orxFASTCALL orxCommand_CommandOr(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* Updates result */
  _pstResult->bValue = _astArgList[0].bValue || _astArgList[1].bValue;

  /* Done! */
  return;
}

/* Command: XOr */
void orxFASTCALL orxCommand_CommandXOr(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* Updates result */
  _pstResult->bValue = (_astArgList[0].bValue || _astArgList[1].bValue) && !(_astArgList[0].bValue && _astArgList[1].bValue);

  /* Done! */
  return;
}

/* Command: AreEqual */
void orxFASTCALL orxCommand_CommandAreEqual(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR astOperandList[2];

  /* Parses numerical arguments */
  if(orxCommand_ParseNumericalArguments(_u32ArgNumber, _astArgList, astOperandList) != orxSTATUS_FAILURE)
  {
    /* Both floats? */
    if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT))
    {
      /* Updates result */
      _pstResult->bValue = (astOperandList[0].fValue == astOperandList[1].fValue) ? orxTRUE : orxFALSE;
    }
    /* Both vectors? */
    else if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_VECTOR)
         && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_VECTOR))
    {
      /* Updates result */
      _pstResult->bValue = orxVector_AreEqual(&(astOperandList[0].vValue), &(astOperandList[1].vValue));
    }
    else
    {
      /* Updates result */
      _pstResult->bValue = (orxString_ICompare(_astArgList[0].zValue, _astArgList[1].zValue) == 0) ? orxTRUE : orxFALSE;
    }
  }
  else
  {
    orxBOOL bOperand1, bOperand2;

    /* Gets bool operands */
    if((orxString_ToBool(_astArgList[0].zValue, &bOperand1, orxNULL) != orxSTATUS_FAILURE)
    && (orxString_ToBool(_astArgList[1].zValue, &bOperand2, orxNULL) != orxSTATUS_FAILURE))
    {
      /* Updates result */
      _pstResult->bValue = (bOperand1 == bOperand2) ? orxTRUE : orxFALSE;
    }
    else
    {
      /* Updates result */
      _pstResult->bValue = (orxString_ICompare(_astArgList[0].zValue, _astArgList[1].zValue) == 0) ? orxTRUE : orxFALSE;
    }
  }

  /* Done! */
  return;
}

/* Command: Add */
void orxFASTCALL orxCommand_CommandAdd(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR astOperandList[2];

  /* Parses numerical arguments */
  if(orxCommand_ParseNumericalArguments(_u32ArgNumber, _astArgList, astOperandList) != orxSTATUS_FAILURE)
  {
    /* Both floats? */
    if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT))
    {
      /* Prints value */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%g", astOperandList[0].fValue + astOperandList[1].fValue);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
    else
    {
      orxVECTOR vResult;

      /* Is operand1 a float? */
      if(astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[0].vValue), astOperandList[0].fValue);
      }
      /* Is operand2 a float? */
      else if(astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[1].vValue), astOperandList[1].fValue);
      }

      /* Updates intermediate result */
      orxVector_Add(&vResult, &(astOperandList[0].vValue), &(astOperandList[1].vValue));

      /* Prints it */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%c%g%c%g%c%g%c", orxSTRING_KC_VECTOR_START, vResult.fX, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fY, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fZ, orxSTRING_KC_VECTOR_END);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/* Command: Subtract */
void orxFASTCALL orxCommand_CommandSubtract(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR astOperandList[2];

  /* Parses numerical arguments */
  if(orxCommand_ParseNumericalArguments(_u32ArgNumber, _astArgList, astOperandList) != orxSTATUS_FAILURE)
  {
    /* Both floats? */
    if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT))
    {
      /* Prints value */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%g", astOperandList[0].fValue - astOperandList[1].fValue);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
    else
    {
      orxVECTOR vResult;

      /* Is operand1 a float? */
      if(astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[0].vValue), astOperandList[0].fValue);
      }
      /* Is operand2 a float? */
      else if(astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[1].vValue), astOperandList[1].fValue);
      }

      /* Updates intermediate result */
      orxVector_Sub(&vResult, &(astOperandList[0].vValue), &(astOperandList[1].vValue));

      /* Prints it */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%c%g%c%g%c%g%c", orxSTRING_KC_VECTOR_START, vResult.fX, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fY, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fZ, orxSTRING_KC_VECTOR_END);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/* Command: Multiply */
void orxFASTCALL orxCommand_CommandMultiply(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR astOperandList[2];

  /* Parses numerical arguments */
  if(orxCommand_ParseNumericalArguments(_u32ArgNumber, _astArgList, astOperandList) != orxSTATUS_FAILURE)
  {
    /* Both floats? */
    if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT))
    {
      /* Prints value */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%g", astOperandList[0].fValue * astOperandList[1].fValue);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
    else
    {
      orxVECTOR vResult;

      /* Is operand1 a float? */
      if(astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[0].vValue), astOperandList[0].fValue);
      }
      /* Is operand2 a float? */
      else if(astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[1].vValue), astOperandList[1].fValue);
      }

      /* Updates intermediate result */
      orxVector_Mul(&vResult, &(astOperandList[0].vValue), &(astOperandList[1].vValue));

      /* Prints it */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%c%g%c%g%c%g%c", orxSTRING_KC_VECTOR_START, vResult.fX, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fY, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fZ, orxSTRING_KC_VECTOR_END);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/* Command: Divide */
void orxFASTCALL orxCommand_CommandDivide(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR astOperandList[2];

  /* Parses numerical arguments */
  if(orxCommand_ParseNumericalArguments(_u32ArgNumber, _astArgList, astOperandList) != orxSTATUS_FAILURE)
  {
    /* Both floats? */
    if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT))
    {
      /* Prints value */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%g", astOperandList[0].fValue / astOperandList[1].fValue);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
    else
    {
      orxVECTOR vResult;

      /* Is operand1 a float? */
      if(astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[0].vValue), astOperandList[0].fValue);
      }
      /* Is operand2 a float? */
      else if(astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[1].vValue), astOperandList[1].fValue);
      }

      /* Updates intermediate result */
      orxVector_Div(&vResult, &(astOperandList[0].vValue), &(astOperandList[1].vValue));

      /* Prints it */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%c%g%c%g%c%g%c", orxSTRING_KC_VECTOR_START, vResult.fX, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fY, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fZ, orxSTRING_KC_VECTOR_END);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/* Command: Minimum */
void orxFASTCALL orxCommand_CommandMinimum(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR astOperandList[2];

  /* Parses numerical arguments */
  if(orxCommand_ParseNumericalArguments(_u32ArgNumber, _astArgList, astOperandList) != orxSTATUS_FAILURE)
  {
    /* Both floats? */
    if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT))
    {
      /* Prints value */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%g", orxMIN(astOperandList[0].fValue, astOperandList[1].fValue));

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
    else
    {
      orxVECTOR vResult;

      /* Is operand1 a float? */
      if(astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[0].vValue), astOperandList[0].fValue);
      }
      /* Is operand2 a float? */
      else if(astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[1].vValue), astOperandList[1].fValue);
      }

      /* Updates intermediate result */
      orxVector_Min(&vResult, &(astOperandList[0].vValue), &(astOperandList[1].vValue));

      /* Prints it */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%c%g%c%g%c%g%c", orxSTRING_KC_VECTOR_START, vResult.fX, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fY, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fZ, orxSTRING_KC_VECTOR_END);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/* Command: Maximum */
void orxFASTCALL orxCommand_CommandMaximum(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR astOperandList[2];

  /* Parses numerical arguments */
  if(orxCommand_ParseNumericalArguments(_u32ArgNumber, _astArgList, astOperandList) != orxSTATUS_FAILURE)
  {
    /* Both floats? */
    if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT))
    {
      /* Prints value */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%g", orxMAX(astOperandList[0].fValue, astOperandList[1].fValue));

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
    else
    {
      orxVECTOR vResult;

      /* Is operand1 a float? */
      if(astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[0].vValue), astOperandList[0].fValue);
      }
      /* Is operand2 a float? */
      else if(astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[1].vValue), astOperandList[1].fValue);
      }

      /* Updates intermediate result */
      orxVector_Max(&vResult, &(astOperandList[0].vValue), &(astOperandList[1].vValue));

      /* Prints it */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%c%g%c%g%c%g%c", orxSTRING_KC_VECTOR_START, vResult.fX, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fY, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fZ, orxSTRING_KC_VECTOR_END);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/* Command: Clamp */
void orxFASTCALL orxCommand_CommandClamp(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR astOperandList[3];

  /* Parses numerical arguments */
  if(orxCommand_ParseNumericalArguments(_u32ArgNumber, _astArgList, astOperandList) != orxSTATUS_FAILURE)
  {
    /* All floats? */
    if((astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT)
    && (astOperandList[2].eType == orxCOMMAND_VAR_TYPE_FLOAT))
    {
      /* Prints value */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%g", orxCLAMP(astOperandList[0].fValue, astOperandList[1].fValue, astOperandList[2].fValue));

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
    else
    {
      orxVECTOR vResult;

      /* Is operand1 a float? */
      if(astOperandList[0].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[0].vValue), astOperandList[0].fValue);
      }
      /* Is operand2 a float? */
      if(astOperandList[1].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[1].vValue), astOperandList[1].fValue);
      }
      /* Is operand3 a float? */
      if(astOperandList[2].eType == orxCOMMAND_VAR_TYPE_FLOAT)
      {
        /* Converts it */
        orxVector_SetAll(&(astOperandList[2].vValue), astOperandList[2].fValue);
      }

      /* Updates intermediate result */
      orxVector_Clamp(&vResult, &(astOperandList[0].vValue), &(astOperandList[1].vValue), &(astOperandList[2].vValue));

      /* Prints it */
      orxString_NPrint(sstCommand.acResultBuffer, orxCOMMAND_KU32_RESULT_BUFFER_SIZE - 1, "%c%g%c%g%c%g%c", orxSTRING_KC_VECTOR_START, vResult.fX, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fY, orxSTRING_KC_VECTOR_SEPARATOR, vResult.fZ, orxSTRING_KC_VECTOR_END);

      /* Updates result */
      _pstResult->zValue = sstCommand.acResultBuffer;
    }
  }
  else
  {
    /* Updates result */
    _pstResult->zValue = orxSTRING_EMPTY;
  }

  /* Done! */
  return;
}

/* Command: Compare */
void orxFASTCALL orxCommand_CommandCompare(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* Updates result */
  _pstResult->s32Value = ((_u32ArgNumber <= 2) || (_astArgList[2].bValue == orxFALSE)) ? orxString_ICompare(_astArgList[0].zValue, _astArgList[1].zValue) : orxString_Compare(_astArgList[0].zValue, _astArgList[1].zValue);

  /* Done! */
  return;
}

/* Command: CRC */
void orxFASTCALL orxCommand_CommandCRC(orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  /* Updates result */
  _pstResult->u32Value = orxString_ToCRC(_astArgList[0].zValue);

  /* Done! */
  return;
}

/** Registers all the command commands
 */
static orxINLINE void orxCommand_RegisterCommands()
{
  /* Command: Help */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Help, "Help", orxCOMMAND_VAR_TYPE_STRING, 0, 1, {"Command = \"\"", orxCOMMAND_VAR_TYPE_STRING});

  /* Command: ListCommands */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, ListCommands, "Counter", orxCOMMAND_VAR_TYPE_U32, 0, 1, {"Prefix = \"\"", orxCOMMAND_VAR_TYPE_STRING});

  /* Command: AddAlias */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, AddAlias, "Alias", orxCOMMAND_VAR_TYPE_STRING, 2, 1, {"Alias", orxCOMMAND_VAR_TYPE_STRING}, {"Command/Alias", orxCOMMAND_VAR_TYPE_STRING}, {"Arguments", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: RemoveAlias */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, RemoveAlias, "Alias", orxCOMMAND_VAR_TYPE_STRING, 1, 0, {"Alias", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: ListAliases */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, ListAliases, "Counter", orxCOMMAND_VAR_TYPE_U32, 0, 1, {"Prefix = \"\"", orxCOMMAND_VAR_TYPE_STRING});

  /* Command: Evaluate */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Evaluate, "Result", orxCOMMAND_VAR_TYPE_STRING, 1, 0, {"Command", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: EvaluateIf */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, EvaluateIf, "Result", orxCOMMAND_VAR_TYPE_STRING, 2, 1, {"Test", orxCOMMAND_VAR_TYPE_STRING}, {"If-Command", orxCOMMAND_VAR_TYPE_STRING}, {"Else-Command = <void>", orxCOMMAND_VAR_TYPE_STRING});

  /* Command: If */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, If, "Select?", orxCOMMAND_VAR_TYPE_STRING, 2, 1, {"Test", orxCOMMAND_VAR_TYPE_STRING}, {"If-Result", orxCOMMAND_VAR_TYPE_STRING}, {"Else-Result = <void>", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: Not */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Not, "Not", orxCOMMAND_VAR_TYPE_BOOL, 1, 0, {"Operand", orxCOMMAND_VAR_TYPE_BOOL});
  /* Command: And */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, And, "And", orxCOMMAND_VAR_TYPE_BOOL, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_BOOL}, {"Operand2", orxCOMMAND_VAR_TYPE_BOOL});
  /* Command: Or */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Or, "Or", orxCOMMAND_VAR_TYPE_BOOL, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_BOOL}, {"Operand2", orxCOMMAND_VAR_TYPE_BOOL});
  /* Command: XOr */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, XOr, "XOr", orxCOMMAND_VAR_TYPE_BOOL, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_BOOL}, {"Operand2", orxCOMMAND_VAR_TYPE_BOOL});
  /* Command: AreEqual */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, AreEqual, "Equal?", orxCOMMAND_VAR_TYPE_BOOL, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_STRING}, {"Operand2", orxCOMMAND_VAR_TYPE_STRING});

  /* Command: Add */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Add, "Result", orxCOMMAND_VAR_TYPE_STRING, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_STRING}, {"Operand2", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: Subtract */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Subtract, "Result", orxCOMMAND_VAR_TYPE_STRING, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_STRING}, {"Operand2", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: Multiply */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Multiply, "Result", orxCOMMAND_VAR_TYPE_STRING, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_STRING}, {"Operand2", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: Divide */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Divide, "Result", orxCOMMAND_VAR_TYPE_STRING, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_STRING}, {"Operand2", orxCOMMAND_VAR_TYPE_STRING});

  /* Command: Minimum */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Minimum, "Result", orxCOMMAND_VAR_TYPE_STRING, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_STRING}, {"Operand2", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: Maximum */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Maximum, "Result", orxCOMMAND_VAR_TYPE_STRING, 2, 0, {"Operand1", orxCOMMAND_VAR_TYPE_STRING}, {"Operand2", orxCOMMAND_VAR_TYPE_STRING});
  /* Command: Clamp */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Clamp, "Result", orxCOMMAND_VAR_TYPE_STRING, 3, 0, {"Value", orxCOMMAND_VAR_TYPE_STRING}, {"Minimum", orxCOMMAND_VAR_TYPE_STRING}, {"Maximum", orxCOMMAND_VAR_TYPE_STRING});

  /* Command: Compare */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, Compare, "Result", orxCOMMAND_VAR_TYPE_S32, 2, 1, {"String1", orxCOMMAND_VAR_TYPE_STRING}, {"String2", orxCOMMAND_VAR_TYPE_STRING}, {"CaseSensitive = false", orxCOMMAND_VAR_TYPE_BOOL});
  /* Command: CRC */
  orxCOMMAND_REGISTER_CORE_COMMAND(Command, CRC, "CRC", orxCOMMAND_VAR_TYPE_U32, 1, 0, {"String", orxCOMMAND_VAR_TYPE_STRING});

  /* Alias: Help */
  orxCommand_AddAlias("Help", "Command.Help", orxNULL);

  /* Alias: Eval */
  orxCommand_AddAlias("Eval", "Command.Evaluate", orxNULL);
  /* Alias: EvalIf */
  orxCommand_AddAlias("EvalIf", "Command.EvaluateIf", orxNULL);

  /* Alias: Logic.If */
  orxCommand_AddAlias("Logic.If", "Command.If", orxNULL);
  /* Alias: Logic.Not */
  orxCommand_AddAlias("Logic.Not", "Command.Not", orxNULL);
  /* Alias: Logic.And */
  orxCommand_AddAlias("Logic.And", "Command.And", orxNULL);
  /* Alias: Logic.Or */
  orxCommand_AddAlias("Logic.Or", "Command.Or", orxNULL);
  /* Alias: Logic.XOr */
  orxCommand_AddAlias("Logic.XOr", "Command.XOr", orxNULL);
  /* Alias: Logic.AreEqual */
  orxCommand_AddAlias("Logic.AreEqual", "Command.AreEqual", orxNULL);

  /* Alias: If */
  orxCommand_AddAlias("If", "Logic.If", orxNULL);
  /* Alias: Not */
  orxCommand_AddAlias("Not", "Logic.Not", orxNULL);
  /* Alias: And */
  orxCommand_AddAlias("And", "Logic.And", orxNULL);
  /* Alias: Or */
  orxCommand_AddAlias("Or", "Logic.Or", orxNULL);
  /* Alias: XOr */
  orxCommand_AddAlias("XOr", "Logic.XOr", orxNULL);
  /* Alias: == */
  orxCommand_AddAlias("==", "Logic.AreEqual", orxNULL);

  /* Alias: Math.Add */
  orxCommand_AddAlias("Math.Add", "Command.Add", orxNULL);
  /* Alias: Math.Sub */
  orxCommand_AddAlias("Math.Sub", "Command.Subtract", orxNULL);
  /* Alias: Math.Mul */
  orxCommand_AddAlias("Math.Mul", "Command.Multiply", orxNULL);
  /* Alias: Math.Div */
  orxCommand_AddAlias("Math.Div", "Command.Divide", orxNULL);

  /* Alias: + */
  orxCommand_AddAlias("+", "Math.Add", orxNULL);
  /* Alias: - */
  orxCommand_AddAlias("-", "Math.Sub", orxNULL);
  /* Alias: * */
  orxCommand_AddAlias("*", "Math.Mul", orxNULL);
  /* Alias: / */
  orxCommand_AddAlias("/", "Math.Div", orxNULL);

  /* Alias: Math.Min */
  orxCommand_AddAlias("Math.Min", "Command.Minimum", orxNULL);
  /* Alias: Math.Max */
  orxCommand_AddAlias("Math.Max", "Command.Maximum", orxNULL);
  /* Alias: Math.Clamp */
  orxCommand_AddAlias("Math.Clamp", "Command.Clamp", orxNULL);

  /* Alias: Min */
  orxCommand_AddAlias("Min", "Math.Min", orxNULL);
  /* Alias: Max */
  orxCommand_AddAlias("Max", "Math.Max", orxNULL);
  /* Alias: Clamp */
  orxCommand_AddAlias("Clamp", "Math.Clamp", orxNULL);

  /* Alias: String.Compare */
  orxCommand_AddAlias("String.Compare", "Command.Compare", orxNULL);
  /* Alias: String.CRC */
  orxCommand_AddAlias("String.CRC", "Command.CRC", orxNULL);
}

/** Unregisters all the command commands
 */
static orxINLINE void orxCommand_UnregisterCommands()
{
  /* Alias: Help */
  orxCommand_RemoveAlias("Help");

  /* Alias: Eval */
  orxCommand_RemoveAlias("Eval");
  /* Alias: EvalIf */
  orxCommand_RemoveAlias("EvalIf");

  /* Alias: Logic.If */
  orxCommand_RemoveAlias("Logic.If");
  /* Alias: Logic.Not */
  orxCommand_RemoveAlias("Logic.Not");
  /* Alias: Logic.And */
  orxCommand_RemoveAlias("Logic.And");
  /* Alias: Logic.Or */
  orxCommand_RemoveAlias("Logic.Or");
  /* Alias: Logic.XOr */
  orxCommand_RemoveAlias("Logic.XOr");
  /* Alias: Logic.AreEqual */
  orxCommand_RemoveAlias("Logic.AreEqual");

  /* Alias: If */
  orxCommand_RemoveAlias("If");
  /* Alias: Not */
  orxCommand_RemoveAlias("Not");
  /* Alias: And */
  orxCommand_RemoveAlias("And");
  /* Alias: Or */
  orxCommand_RemoveAlias("Or");
  /* Alias: XOr */
  orxCommand_RemoveAlias("XOr");
  /* Alias: == */
  orxCommand_RemoveAlias("==");

  /* Alias: Math.Add */
  orxCommand_RemoveAlias("Math.Add");
  /* Alias: Math.Sub */
  orxCommand_RemoveAlias("Math.Sub");
  /* Alias: Math.Mul */
  orxCommand_RemoveAlias("Math.Mul");
  /* Alias: Math.Div */
  orxCommand_RemoveAlias("Math.Div");

  /* Alias: + */
  orxCommand_RemoveAlias("+");
  /* Alias: - */
  orxCommand_RemoveAlias("-");
  /* Alias: * */
  orxCommand_RemoveAlias("*");
  /* Alias: / */
  orxCommand_RemoveAlias("/");

  /* Alias: Math.Min */
  orxCommand_RemoveAlias("Math.Min");
  /* Alias: Math.Max */
  orxCommand_RemoveAlias("Math.Max");
  /* Alias: Math.Clamp */
  orxCommand_RemoveAlias("Math.Clamp");

  /* Alias: Min */
  orxCommand_RemoveAlias("Min");
  /* Alias: Max */
  orxCommand_RemoveAlias("Max");
  /* Alias: Clamp */
  orxCommand_RemoveAlias("Clamp");

  /* Alias: Compare */
  orxCommand_RemoveAlias("String.Compare");
  /* Alias: CRC */
  orxCommand_RemoveAlias("String.CRC");

  /* Command: Help */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Help);

  /* Command: ListCommands */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, ListCommands);

  /* Command: AddAlias */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, AddAlias);
  /* Command: RemoveAlias */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, RemoveAlias);
  /* Command: ListAliases */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, ListAliases);

  /* Command: Evaluate */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Evaluate);
  /* Command: EvaluateIf */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, EvaluateIf);

  /* Command: If */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, If);
  /* Command: Not */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Not);
  /* Command: And */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, And);
  /* Command: Or */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Or);
  /* Command: XOr */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, XOr);

  /* Command: Add */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Add);
  /* Command: Subtract */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Subtract);
  /* Command: Multiply */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Multiply);
  /* Command: Divide */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Divide);

  /* Command: Minimum */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Minimum);
  /* Command: Maximum */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Maximum);
  /* Command: Clamp */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Clamp);

  /* Command: Compare */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, Compare);
  /* Command: CRC */
  orxCOMMAND_UNREGISTER_CORE_COMMAND(Command, CRC);
}

/***************************************************************************
 * Public functions                                                        *
 ***************************************************************************/

/** Command module setup
 */
void orxFASTCALL orxCommand_Setup()
{
  /* Adds module dependencies */
  orxModule_AddDependency(orxMODULE_ID_COMMAND, orxMODULE_ID_MEMORY);
  orxModule_AddDependency(orxMODULE_ID_COMMAND, orxMODULE_ID_BANK);
  orxModule_AddDependency(orxMODULE_ID_COMMAND, orxMODULE_ID_EVENT);
  orxModule_AddDependency(orxMODULE_ID_COMMAND, orxMODULE_ID_PROFILER);

  /* Done! */
  return;
}

/** Inits command module
 * @return orxSTATUS_SUCCESS / orxSTATUS_FAILURE
 */
orxSTATUS orxFASTCALL orxCommand_Init()
{
  orxSTATUS eResult = orxSTATUS_FAILURE;

  /* Not already Initialized? */
  if(!(sstCommand.u32Flags & orxCOMMAND_KU32_STATIC_FLAG_READY))
  {
    /* Cleans control structure */
    orxMemory_Zero(&sstCommand, sizeof(orxCOMMAND_STATIC));

    /* Registers event handler */
    if(orxEvent_AddHandler(orxEVENT_TYPE_TIMELINE, orxCommand_EventHandler) != orxSTATUS_FAILURE)
    {
      /* Creates banks */
      sstCommand.pstBank        = orxBank_Create(orxCOMMAND_KU32_BANK_SIZE, sizeof(orxCOMMAND), orxBANK_KU32_FLAG_NONE, orxMEMORY_TYPE_MAIN);
      sstCommand.pstTrieBank    = orxBank_Create(orxCOMMAND_KU32_TRIE_BANK_SIZE, sizeof(orxCOMMAND_TRIE_NODE), orxBANK_KU32_FLAG_NONE, orxMEMORY_TYPE_MAIN);
      sstCommand.pstResultBank  = orxBank_Create(orxCOMMAND_KU32_RESULT_BANK_SIZE, sizeof(orxCOMMAND_STACK_ENTRY), orxBANK_KU32_FLAG_NONE, orxMEMORY_TYPE_MAIN);

      /* Valid? */
      if((sstCommand.pstBank != orxNULL) && (sstCommand.pstTrieBank != orxNULL) && (sstCommand.pstResultBank != orxNULL))
      {
        orxCOMMAND_TRIE_NODE *pstTrieRoot;

        /* Allocates trie root */
        pstTrieRoot = (orxCOMMAND_TRIE_NODE *)orxBank_Allocate(sstCommand.pstTrieBank);

        /* Success? */
        if(pstTrieRoot != orxNULL)
        {
          /* Inits it */
          orxMemory_Zero(pstTrieRoot, sizeof(orxCOMMAND_TRIE_NODE));

          /* Adds it to the trie */
          if(orxTree_AddRoot(&(sstCommand.stCommandTrie), &(pstTrieRoot->stNode)) != orxSTATUS_FAILURE)
          {
            /* Inits Flags */
            sstCommand.u32Flags = orxCOMMAND_KU32_STATIC_FLAG_READY;

            /* Registers commands */
            orxCommand_RegisterCommands();

            /* Inits buffers */
            sstCommand.acEvaluateBuffer[orxCOMMAND_KU32_EVALUATE_BUFFER_SIZE - 1]   = orxCHAR_NULL;
            sstCommand.acPrototypeBuffer[orxCOMMAND_KU32_PROTOTYPE_BUFFER_SIZE - 1] = orxCHAR_NULL;

            /* Updates result */
            eResult = orxSTATUS_SUCCESS;
          }
        }

        /* Failure? */
        if(eResult == orxSTATUS_FAILURE)
        {
          /* Removes event handler */
          orxEvent_RemoveHandler(orxEVENT_TYPE_TIMELINE, orxCommand_EventHandler);

          /* Deletes banks */
          orxBank_Delete(sstCommand.pstBank);
          orxBank_Delete(sstCommand.pstTrieBank);
          orxBank_Delete(sstCommand.pstResultBank);

          /* Logs message */
          orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Failed to create command trie.");
        }
      }
      else
      {
        /* Partly initialized? */
        if(sstCommand.pstTrieBank != orxNULL)
        {
          /* Deletes bank */
          orxBank_Delete(sstCommand.pstTrieBank);
        }

        /* Partly initialized? */
        if(sstCommand.pstBank != orxNULL)
        {
          /* Deletes bank */
          orxBank_Delete(sstCommand.pstBank);
        }

        /* Removes event handler */
        orxEvent_RemoveHandler(orxEVENT_TYPE_TIMELINE, orxCommand_EventHandler);

        /* Logs message */
        orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Failed to create command banks.");
      }
    }
    else
    {
      /* Logs message */
      orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Failed to register event handler.");
    }
  }
  else
  {
    /* Logs message */
    orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Tried to initialize command module when it was already initialized.");

    /* Already initialized */
    eResult = orxSTATUS_SUCCESS;
  }

  /* Done! */
  return eResult;
}

/** Exits from command module
 */
void orxFASTCALL orxCommand_Exit()
{
  /* Initialized? */
  if(sstCommand.u32Flags & orxCOMMAND_KU32_STATIC_FLAG_READY)
  {
    orxCOMMAND *pstCommand;
    orxU32      i;

    /* Unregisters commands */
    orxCommand_UnregisterCommands();

    /* For all entries in the result stack */
    for(i = 0; i < orxBank_GetCounter(sstCommand.pstResultBank); i++)
    {
      orxCOMMAND_STACK_ENTRY *pstEntry;

      /* Gets it */
      pstEntry = (orxCOMMAND_STACK_ENTRY *)orxBank_GetAtIndex(sstCommand.pstResultBank, i);

      /* Is a string value? */
      if(pstEntry->stValue.eType == orxCOMMAND_VAR_TYPE_STRING)
      {
        /* Deletes it */
        orxString_Delete((orxCHAR *)pstEntry->stValue.zValue);
      }
    }

    /* For all registered commands */
    for(pstCommand = (orxCOMMAND *)orxBank_GetNext(sstCommand.pstBank, orxNULL);
        pstCommand != orxNULL;
        pstCommand = (orxCOMMAND *)orxBank_GetNext(sstCommand.pstBank, pstCommand))
    {
      /* Deletes its name */
      orxString_Delete(pstCommand->zName);
    }

    /* Clears trie */
    orxTree_Clean(&(sstCommand.stCommandTrie));

    /* Deletes banks */
    orxBank_Delete(sstCommand.pstBank);
    orxBank_Delete(sstCommand.pstTrieBank);
    orxBank_Delete(sstCommand.pstResultBank);

    /* Removes event handler */
    orxEvent_RemoveHandler(orxEVENT_TYPE_TIMELINE, orxCommand_EventHandler);

    /* Updates flags */
    sstCommand.u32Flags &= ~orxCOMMAND_KU32_STATIC_FLAG_READY;
  }

  /* Done! */
  return;
}

/** Registers a command
* @param[in]   _zCommand      Command name
* @param[in]   _pfnFunction   Associated function
* @param[in]   _u32RequiredParamNumber Number of required parameters of the command
* @param[in]   _u32OptionalParamNumber Number of optional parameters of the command
* @param[in]   _astParamList  List of parameters of the command
* @param[in]   _pstResult     Result
* @return      orxSTATUS_SUCCESS / orxSTATUS_FAILURE
*/
orxSTATUS orxFASTCALL orxCommand_Register(const orxSTRING _zCommand, const orxCOMMAND_FUNCTION _pfnFunction, orxU32 _u32RequiredParamNumber, orxU32 _u32OptionalParamNumber, const orxCOMMAND_VAR_DEF *_astParamList, const orxCOMMAND_VAR_DEF *_pstResult)
{
  orxSTATUS eResult = orxSTATUS_FAILURE;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zCommand != orxNULL);
  orxASSERT(_pfnFunction != orxNULL);
  orxASSERT(_u32RequiredParamNumber <= 0xFFFF);
  orxASSERT(_u32OptionalParamNumber <= 0xFFFF);
  orxASSERT(_astParamList != orxNULL);
  orxASSERT(_pstResult != orxNULL);

  /* Valid? */
  if((_zCommand != orxNULL) && (*_zCommand != orxCHAR_NULL))
  {
    /* Not already registered? */
    if(orxCommand_FindNoAlias(_zCommand) == orxNULL)
    {
      orxCOMMAND *pstCommand;

      /* Allocates command */
      pstCommand = (orxCOMMAND *)orxBank_Allocate(sstCommand.pstBank);

      /* Valid? */
      if(pstCommand != orxNULL)
      {
        orxU32 i;

        /* Inits it */
        orxMemory_Zero(pstCommand, sizeof(orxCOMMAND));
        pstCommand->stResult.zName          = orxString_Duplicate(_pstResult->zName);
        pstCommand->bIsAlias                = orxFALSE;
        pstCommand->pfnFunction             = _pfnFunction;
        pstCommand->stResult.eType          = _pstResult->eType;
        pstCommand->zName                   = orxString_Duplicate(_zCommand);
        pstCommand->u16RequiredParamNumber  = (orxU16)_u32RequiredParamNumber;
        pstCommand->u16OptionalParamNumber  = (orxU16)_u32OptionalParamNumber;

        /* Allocates parameter list */
        pstCommand->astParamList = (orxCOMMAND_VAR_DEF *)orxMemory_Allocate((_u32RequiredParamNumber + _u32OptionalParamNumber) * sizeof(orxCOMMAND_VAR_DEF), orxMEMORY_TYPE_MAIN);

        /* Checks */
        orxASSERT(pstCommand->astParamList != orxNULL);

        /* For all parameters */
        for(i = 0; i < _u32RequiredParamNumber + _u32OptionalParamNumber; i++)
        {
          /* Inits it */
          pstCommand->astParamList[i].zName = orxString_Duplicate(_astParamList[i].zName);
          pstCommand->astParamList[i].eType = _astParamList[i].eType;
        }

        /* Inserts in trie */
        orxCommand_InsertInTrie(pstCommand);

        /* Updates result */
        eResult = orxSTATUS_SUCCESS;
      }
      else
      {
        /* Logs message */
        orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't allocate memory for command [%s], aborting.", _zCommand);
      }
    }
    else
    {
      /* Logs message */
      orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't register command: [%s] is already registered.", _zCommand);
    }
  }

  /* Done! */
  return eResult;
}

/** Unregisters a command
* @param[in]   _zCommand      Command name
* @return      orxSTATUS_SUCCESS / orxSTATUS_FAILURE
*/
orxSTATUS orxFASTCALL orxCommand_Unregister(const orxSTRING _zCommand)
{
  orxSTATUS eResult = orxSTATUS_FAILURE;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zCommand != orxNULL);

  /* Valid? */
  if(_zCommand != orxNULL)
  {
    orxCOMMAND *pstCommand;

    /* Gets it */
    pstCommand = orxCommand_FindNoAlias(_zCommand);

    /* Found? */
    if(pstCommand != orxNULL)
    {
      orxU32 i;

      /* Removes it from trie */
      orxCommand_RemoveFromTrie(pstCommand);

      /* For all its parameters */
      for(i = 0; i < (orxU32)pstCommand->u16RequiredParamNumber + (orxU32)pstCommand->u16OptionalParamNumber; i++)
      {
        /* Deletes its name */
        orxString_Delete((orxSTRING)pstCommand->astParamList[i].zName);
      }

      /* Deletes its variables */
      orxString_Delete((orxSTRING)pstCommand->stResult.zName);
      orxString_Delete(pstCommand->zName);
      orxMemory_Free(pstCommand->astParamList);

      /* Deletes it */
      orxBank_Free(sstCommand.pstBank, pstCommand);
    }
    else
    {
      /* Logs message */
      orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't unregister command: [%s] is not registered.", _zCommand);
    }
  }

  /* Done! */
  return eResult;
}

/** Is a command registered?
* @param[in]   _zCommand      Command name
* @return      orxTRUE / orxFALSE
*/
orxBOOL orxFASTCALL orxCommand_IsRegistered(const orxSTRING _zCommand)
{
  orxBOOL bResult;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zCommand != orxNULL);

  /* Valid? */
  if(_zCommand != orxNULL)
  {
    /* Updates result */
    bResult = (orxCommand_FindNoAlias(_zCommand) != orxNULL) ? orxTRUE : orxFALSE;
  }
  else
  {
    /* Updates result */
    bResult = orxFALSE;
  }

  /* Done! */
  return bResult;
}

/** Adds a command alias
* @param[in]   _zAlias        Command alias
* @param[in]   _zCommand      Command name
* @param[in]   _zArgs         Command argument, orxNULL for none
* @return      orxSTATUS_SUCCESS / orxSTATUS_FAILURE
*/
orxSTATUS orxFASTCALL orxCommand_AddAlias(const orxSTRING _zAlias, const orxSTRING _zCommand, const orxSTRING _zArgs)
{
  const orxSTRING zAlias;
  orxSTATUS       eResult = orxSTATUS_FAILURE;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zAlias != orxNULL);
  orxASSERT(_zCommand != orxNULL);

  /* Finds start of alias */
  zAlias = orxString_SkipWhiteSpaces(_zAlias);

  /* Valid? */
  if((zAlias != orxNULL) && (*zAlias != orxCHAR_NULL))
  {
    orxCOMMAND_TRIE_NODE *pstAliasNode;

    /* Finds alias node */
    pstAliasNode = orxCommand_FindTrieNode(zAlias, orxTRUE);

    /* Valid? */
    if(pstAliasNode != orxNULL)
    {
      /* Not already used as a command? */
      if((pstAliasNode->pstCommand == orxNULL) || (pstAliasNode->pstCommand->bIsAlias != orxFALSE))
      {
        /* Not self referencing? */
        if(orxString_Compare(zAlias, _zCommand) != 0)
        {
          orxCOMMAND_TRIE_NODE *pstNode;

          /* Updates result */
          eResult = orxSTATUS_SUCCESS;

          /* For all aliases */
          for(pstNode = orxCommand_FindTrieNode(_zCommand, orxFALSE);
              (pstNode != orxNULL) && (pstNode->pstCommand != orxNULL) && (pstNode->pstCommand->bIsAlias != orxFALSE);
              pstNode = orxCommand_FindTrieNode(pstNode->pstCommand->zAliasedCommandName, orxFALSE))
          {
            /* Creates a loop? */
            if(orxString_Compare(zAlias, pstNode->pstCommand->zAliasedCommandName) == 0)
            {
              /* Updates result */
              eResult = orxSTATUS_FAILURE;

              break;
            }
          }
        }

        /* Valid? */
        if(eResult != orxSTATUS_FAILURE)
        {
          /* Not existing? */
          if(pstAliasNode->pstCommand == orxNULL)
          {
            /* Creates it */
            pstAliasNode->pstCommand = (orxCOMMAND *)orxBank_Allocate(sstCommand.pstBank);

            /* Valid? */
            if(pstAliasNode->pstCommand != orxNULL)
            {
              /* Inits */
              orxMemory_Zero(pstAliasNode->pstCommand, sizeof(orxCOMMAND));
              pstAliasNode->pstCommand->zName     = orxString_Duplicate(zAlias);
              pstAliasNode->pstCommand->bIsAlias  = orxTRUE;
            }
            else
            {
              /* Logs message */
              orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't allocate memory for alias [%s], aborting.", zAlias);

              /* Updates result */
              eResult = orxSTATUS_FAILURE;
            }
          }
          else
          {
            /* Logs message */
            orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Updating alias [%s]: now pointing to [%s], previously [%s].", zAlias, _zCommand, pstAliasNode->pstCommand->zAliasedCommandName);

            /* Delete old aliased name */
            orxString_Delete(pstAliasNode->pstCommand->zAliasedCommandName);

            /* Had arguments? */
            if(pstAliasNode->pstCommand->zArgs != orxNULL)
            {
              /* Deletes them */
              orxString_Delete(pstAliasNode->pstCommand->zArgs);
            }
          }

          /* Success? */
          if(eResult != orxSTATUS_FAILURE)
          {
            /* Updates aliased name */
            pstAliasNode->pstCommand->zAliasedCommandName = orxString_Duplicate(_zCommand);
            pstAliasNode->pstCommand->zArgs               = (_zArgs != orxCHAR_NULL) ? orxString_Duplicate(_zArgs) : orxNULL;
          }
        }
        else
        {
          /* Logs message */
          orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't add/modify alias [%s] -> [%s] as it's creating a loop, aborting.", zAlias, _zCommand);
        }
      }
      else
      {
        /* Logs message */
        orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Failed to add alias: [%s] is already registered as a command.", zAlias);
      }
    }
    else
    {
      /* Logs message */
      orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Failed to add alias [%s]: couldn't insert it in trie.", zAlias);
    }
  }

  /* Done! */
  return eResult;
}

/** Removes a command alias
* @param[in]   _zAlias        Command alias
* @return      orxSTATUS_SUCCESS / orxSTATUS_FAILURE
*/
orxSTATUS orxFASTCALL orxCommand_RemoveAlias(const orxSTRING _zAlias)
{
  orxSTATUS eResult = orxSTATUS_FAILURE;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zAlias != orxNULL);

  /* Valid? */
  if(_zAlias != orxNULL)
  {
    orxCOMMAND_TRIE_NODE *pstNode;

    /* Finds its node */
    pstNode = orxCommand_FindTrieNode(_zAlias, orxFALSE);

    /* Success? */
    if(pstNode != orxNULL)
    {
      /* Is an alias? */
      if((pstNode->pstCommand != orxNULL) && (pstNode->pstCommand->bIsAlias != orxFALSE))
      {
        /* Deletes its names */
        orxString_Delete(pstNode->pstCommand->zName);
        orxString_Delete(pstNode->pstCommand->zAliasedCommandName);

        /* Has arguments? */
        if(pstNode->pstCommand->zArgs != orxNULL)
        {
          /* Deletes it */
          orxString_Delete(pstNode->pstCommand->zArgs);
        }

        /* Deletes it */
        orxBank_Free(sstCommand.pstBank, pstNode->pstCommand);

        /* Removes its reference */
        pstNode->pstCommand = orxNULL;

        /* Updates result */
        eResult = orxSTATUS_SUCCESS;
      }
      else
      {
        /* Logs message */
        orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Failed to remove alias: [%s] is a command, not an alias.", _zAlias);
      }
    }
    else
    {
      /* Logs message */
      orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Failed to remove alias: [%s] alias not found.", _zAlias);
    }
  }

  /* Done! */
  return eResult;
}

/** Is a command alias?
* @param[in]   _zAlias        Command alias
* @return      orxTRUE / orxFALSE
*/
orxBOOL orxFASTCALL orxCommand_IsAlias(const orxSTRING _zAlias)
{
  orxBOOL bResult = orxFALSE;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zAlias != orxNULL);

  /* Valid? */
  if(_zAlias != orxNULL)
  {
    orxCOMMAND_TRIE_NODE *pstNode;

    /* Finds its node */
    pstNode = orxCommand_FindTrieNode(_zAlias, orxFALSE);

    /* Success? */
    if(pstNode != orxNULL)
    {
      /* Is an alias? */
      if((pstNode->pstCommand != orxNULL) && (pstNode->pstCommand->bIsAlias != orxFALSE))
      {
        /* Updates result */
        bResult = orxTRUE;
      }
    }
  }

  /* Done! */
  return bResult;
}

/** Gets a command's (text) prototype (beware: result won't persist from one call to the other)
* @param[in]   _zCommand      Command name
* @return      Command prototype / orxSTRING_EMPTY
*/
const orxSTRING orxFASTCALL orxCommand_GetPrototype(const orxSTRING _zCommand)
{
  const orxSTRING zResult = orxSTRING_EMPTY;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zCommand != orxNULL);

  /* Valid? */
  if((_zCommand != orxNULL) & (_zCommand != orxSTRING_EMPTY))
  {
    orxCOMMAND *pstCommand;

    /* Gets command */
    pstCommand = orxCommand_FindNoAlias(_zCommand);

    /* Success? */
    if(pstCommand != orxNULL)
    {
      orxU32 i, u32Size;

      /* Prints result and function name */
      u32Size = orxString_NPrint(sstCommand.acPrototypeBuffer, orxCOMMAND_KU32_PROTOTYPE_BUFFER_SIZE - 1, "{%s %s} %s", orxCommand_GetTypeString(pstCommand->stResult.eType), pstCommand->stResult.zName, pstCommand->zName);

      /* For all required arguments */
      for(i = 0; i < (orxU32)pstCommand->u16RequiredParamNumber; i++)
      {
        /* Prints it */
        u32Size += orxString_NPrint(sstCommand.acPrototypeBuffer + u32Size, orxCOMMAND_KU32_PROTOTYPE_BUFFER_SIZE - 1 - u32Size, " (%s %s)", orxCommand_GetTypeString(pstCommand->astParamList[i].eType), pstCommand->astParamList[i].zName);
      }

      /* For all optional arguments */
      for(; i < (orxU32)pstCommand->u16RequiredParamNumber + (orxU32)pstCommand->u16OptionalParamNumber; i++)
      {
        /* Prints it */
        u32Size += orxString_NPrint(sstCommand.acPrototypeBuffer + u32Size, orxCOMMAND_KU32_PROTOTYPE_BUFFER_SIZE - 1 - u32Size, " [%s %s]", orxCommand_GetTypeString(pstCommand->astParamList[i].eType), pstCommand->astParamList[i].zName);
      }

      /* Had no parameters? */
      if(i == 0)
      {
        /* Prints function end */
        u32Size += orxString_NPrint(sstCommand.acPrototypeBuffer + u32Size, orxCOMMAND_KU32_PROTOTYPE_BUFFER_SIZE - 1 - u32Size, " <void>");
      }

      /* Updates result */
      zResult = sstCommand.acPrototypeBuffer;
    }
  }

  /* Done! */
  return zResult;
}

/** Gets next command using an optional base
* @param[in]   _zBase             Base name, can be set to orxNULL for no base
* @param[in]   _zPrevious         Previous command, orxNULL to get the first command
* @param[out]  _pu32CommonLength  Length of the common prefix of all potential results
* @return      Next command found, orxNULL if none
*/
const orxSTRING orxFASTCALL orxCommand_GetNext(const orxSTRING _zBase, const orxSTRING _zPrevious, orxU32 *_pu32CommonLength)
{
  orxCOMMAND_TRIE_NODE *pstBaseNode;
  const orxSTRING       zResult = orxNULL;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));

  /* Has a base? */
  if(_zBase != orxNULL)
  {
    /* Finds base node */
    pstBaseNode = orxCommand_FindTrieNode(_zBase, orxFALSE);

    /* Inits common length */
    if(_pu32CommonLength != orxNULL)
    {
      *_pu32CommonLength = orxString_GetLength(_zBase);
    }
  }
  else
  {
    /* Uses root as base node */
    pstBaseNode = (orxCOMMAND_TRIE_NODE *)orxTree_GetRoot(&(sstCommand.stCommandTrie));

    /* Clears common length */
    if(_pu32CommonLength != orxNULL)
    {
      *_pu32CommonLength = 0;
    }
  }

  /* Found a valid base? */
  if(pstBaseNode != orxNULL)
  {
    orxCOMMAND_TRIE_NODE *pstPreviousNode;
    const orxCOMMAND     *pstNextCommand;

    /* Has previous command? */
    if(_zPrevious != orxNULL)
    {
      /* Gets its node */
      pstPreviousNode = orxCommand_FindTrieNode(_zPrevious, orxFALSE);

      /* Found? */
      if((pstPreviousNode != orxNULL) && (pstPreviousNode->pstCommand != orxNULL))
      {
        /* Different than base? */
        if(pstPreviousNode != pstBaseNode)
        {
          orxCOMMAND_TRIE_NODE *pstParent;

          /* Finds parent base node */
          for(pstParent = (orxCOMMAND_TRIE_NODE *)orxTree_GetParent(&(pstPreviousNode->stNode));
              (pstParent != orxNULL) && (pstParent != pstBaseNode);
              pstParent = (orxCOMMAND_TRIE_NODE *)orxTree_GetParent(&(pstParent->stNode)));

          /* Not found? */
          if(pstParent == orxNULL)
          {
            /* Logs message */
            orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "[%s] is not a valid base of command [%s]: ignoring previous command parameter.", _zBase, _zPrevious);

            /* Resets previous command node */
            pstPreviousNode = orxNULL;
          }
        }
      }
      else
      {
        /* Logs message */
        orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "[%s] is not a valid previous command: ignoring previous command parameter.", _zPrevious);
      }
    }
    else
    {
      /* Ignores previous command */
      pstPreviousNode = orxNULL;
    }

    /* Is child of base valid? */
    if(orxTree_GetChild(&(pstBaseNode->stNode)) != orxNULL)
    {
      /* Finds next command */
      pstNextCommand = orxCommand_FindNext((orxCOMMAND_TRIE_NODE *)orxTree_GetChild(&(pstBaseNode->stNode)), &pstPreviousNode);

      /* Found? */
      if(pstNextCommand != orxNULL)
      {
        orxCOMMAND_TRIE_NODE *pstNode, *pstParent;
        orxU32                i, u32Position;

        /* Finds prefix node position */
        for(pstNode = orxCommand_FindTrieNode(pstNextCommand->zName, orxFALSE), pstParent = (orxCOMMAND_TRIE_NODE *)orxTree_GetParent(&(pstNode->stNode)), i = 0, u32Position = -1;
            pstNode != pstBaseNode;
            pstNode = pstParent, pstParent = (orxCOMMAND_TRIE_NODE *)orxTree_GetParent(&(pstNode->stNode)), i++)
        {
          /* Has sibling? */
          if((orxTree_GetSibling(&(pstNode->stNode)) != orxNULL) || ((orxCOMMAND_TRIE_NODE *)orxTree_GetChild(&(pstParent->stNode)) != pstNode))
          {
            /* Updates position */
            u32Position = i;
          }
        }

        /* Updates prefix length */
        if(_pu32CommonLength != orxNULL)
        {
          *_pu32CommonLength = orxString_GetLength(pstNextCommand->zName) - u32Position - 1;
        }
      }
    }
    else
    {
      /* Gets next command */
      pstNextCommand = (pstBaseNode != pstPreviousNode) ? pstBaseNode->pstCommand : orxNULL;
    }

    /* Found? */
    if(pstNextCommand != orxNULL)
    {
      /* Updates result */
      zResult = pstNextCommand->zName;
    }
    else
    {
      /* Clears common length */
      if(_pu32CommonLength != orxNULL)
      {
        *_pu32CommonLength = 0;
      }
    }
  }
  else
  {
    /* Logs message */
    orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Failed to get next command using base [%s]: base not found.", _zBase);
  }

  /* Done! */
  return zResult;
}

/** Evaluates a command
* @param[in]   _zCommandLine  Command name + arguments
* @param[out]  _pstResult     Variable that will contain the result
* @return      Command result if found, orxNULL otherwise
*/
orxCOMMAND_VAR *orxFASTCALL orxCommand_Evaluate(const orxSTRING _zCommandLine, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR *pstResult = orxNULL;

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zCommandLine != orxNULL);
  orxASSERT(_pstResult != orxNULL);

  /* Valid? */
  if((_zCommandLine != orxNULL) & (_zCommandLine != orxSTRING_EMPTY))
  {
    /* Processes it */
    pstResult = orxCommand_Process(_zCommandLine, orxU64_UNDEFINED, _pstResult);
  }

  /* Done! */
  return pstResult;
}

/** Executes a command
* @param[in]   _zCommand      Command name
* @param[in]   _u32ArgNumber  Number of arguments sent to the command
* @param[in]   _astArgList    List of arguments sent to the command
* @param[out]  _pstResult     Variable that will contain the result
* @return      Command result if found, orxNULL otherwise
*/
orxCOMMAND_VAR *orxFASTCALL orxCommand_Execute(const orxSTRING _zCommand, orxU32 _u32ArgNumber, const orxCOMMAND_VAR *_astArgList, orxCOMMAND_VAR *_pstResult)
{
  orxCOMMAND_VAR *pstResult = orxNULL;

  /* Profiles */
  orxPROFILER_PUSH_MARKER("orxCommand_Execute");

  /* Checks */
  orxASSERT(orxFLAG_TEST(sstCommand.u32Flags, orxCOMMAND_KU32_STATIC_FLAG_READY));
  orxASSERT(_zCommand != orxNULL);
  orxASSERT(_pstResult != orxNULL);

  /* Valid? */
  if((_zCommand != orxNULL) & (_zCommand != orxSTRING_EMPTY))
  {
    orxCOMMAND_TRIE_NODE *pstCommandNode;

    /* Gets its node */
    pstCommandNode = orxCommand_FindTrieNode(_zCommand, orxFALSE);

    /* Found? */
    if((pstCommandNode != orxNULL) && (pstCommandNode->pstCommand != orxNULL))
    {
      /* Not an alias? */
      if(pstCommandNode->pstCommand->bIsAlias == orxFALSE)
      {
        /* Runs it */
        pstResult = orxCommand_Run(pstCommandNode->pstCommand, orxTRUE, _u32ArgNumber, _astArgList, _pstResult);
      }
      else
      {
        /* Logs message */
        orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't execute command: [%s] is an alias, not a command.", _zCommand);
      }
    }
    else
    {
      /* Logs message */
      orxDEBUG_PRINT(orxDEBUG_LEVEL_SYSTEM, "Can't execute command: [%s] is not registered.", _zCommand);
    }
  }

  /* Profiles */
  orxPROFILER_POP_MARKER();

  /* Done! */
  return pstResult;
}

#ifdef __orxMSVC__

  #pragma warning(default : 4200)

#endif /* __orxMSVC__ */
