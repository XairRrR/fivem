/* Exploration : GetEffectiveRightsFromAclW, code de retour pour une ACL non canonique et
 * appartenance aux groupes (alias SAM, groupes bien connus du jeton). Pas de PASS/FAIL. */
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>

static BYTE acl_buf[1024];

static void one( const char *what, PSID me, void (*build)(PACL, PSID) )
{
    TRUSTEE_W trustee;
    ACCESS_MASK mask = 0xdeadbeef;
    DWORD ret;
    InitializeAcl( (PACL)acl_buf, sizeof(acl_buf), ACL_REVISION );
    build( (PACL)acl_buf, me );
    BuildTrusteeWithSidW( &trustee, me );
    ret = GetEffectiveRightsFromAclW( (PACL)acl_buf, &trustee, &mask );
    printf( "%-40s ret %lu mask %#lx\n", what, ret, mask );
}

static PSID group;
static void allow_then_deny( PACL acl, PSID me ) { AddAccessAllowedAce( acl, ACL_REVISION, FILE_ALL_ACCESS, me ); AddAccessDeniedAce( acl, ACL_REVISION, FILE_WRITE_DATA, me ); }
static void allow_group( PACL acl, PSID me ) { AddAccessAllowedAce( acl, ACL_REVISION, FILE_GENERIC_EXECUTE, group ); }
static void deny_group( PACL acl, PSID me ) { AddAccessDeniedAce( acl, ACL_REVISION, FILE_WRITE_DATA, group ); AddAccessAllowedAce( acl, ACL_REVISION, FILE_ALL_ACCESS, me ); }

int main(void)
{
    BYTE user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)user_buf;
    static const char *groups[] = { "S-1-1-0", "S-1-5-11", "S-1-5-4", "S-1-5-32-544", "S-1-5-32-545", "S-1-5-32-546",
                                    "S-1-2-0", "S-1-5-18" };
    HANDLE token;
    DWORD size, i;

    setvbuf( stdout, NULL, _IONBF, 0 );
    OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token );
    GetTokenInformation( token, TokenUser, user, sizeof(user_buf), &size );
    {
        TOKEN_GROUPS *tg;
        GetTokenInformation( token, TokenGroups, NULL, 0, &size );
        tg = malloc( size );
        GetTokenInformation( token, TokenGroups, tg, size, &size );
        for (i = 0; i < tg->GroupCount; i++)
        {
            char *s;
            ConvertSidToStringSidA( tg->Groups[i].Sid, &s );
            printf( "token group %s attributes %#lx\n", s, tg->Groups[i].Attributes );
            LocalFree( s );
        }
    }
    CloseHandle( token );

    one( "allow ALL then deny WRITE_DATA", user->User.Sid, allow_then_deny );
    for (i = 0; i < ARRAYSIZE(groups); i++)
    {
        char what[64];
        ConvertStringSidToSidA( groups[i], &group );
        snprintf( what, sizeof(what), "allow EXECUTE to %s", groups[i] );
        one( what, user->User.Sid, allow_group );
        snprintf( what, sizeof(what), "deny WRITE to %s, allow ALL to me", groups[i] );
        one( what, user->User.Sid, deny_group );
        LocalFree( group );
    }
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
