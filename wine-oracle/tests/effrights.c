/* GetEffectiveRightsFromAclW (mono) : droits accordés par une ACL à un trustee, en tenant compte de ses
 * groupes ; les ACE sont prises dans l'ordre (la première qui nomme un droit décide), les ACE
 * « inherit only » ne s'appliquent pas. Wine renvoyait une erreur sans rien remplir. */
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

static BYTE acl_buf[1024];

static PACL new_acl(void)
{
    InitializeAcl( (PACL)acl_buf, sizeof(acl_buf), ACL_REVISION );
    return (PACL)acl_buf;
}

static PSID sid_of( const char *str )
{
    PSID sid = NULL;
    ConvertStringSidToSidA( str, &sid );
    return sid;
}

static ACCESS_MASK rights( PACL acl, PSID sid, DWORD *ret )
{
    TRUSTEE_W trustee;
    ACCESS_MASK mask = 0xdeadbeef;
    BuildTrusteeWithSidW( &trustee, sid );
    *ret = GetEffectiveRightsFromAclW( acl, &trustee, &mask );
    return mask;
}

int main(void)
{
    BYTE user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)user_buf;
    PSID me, everyone = sid_of( "S-1-1-0" ), system = sid_of( "S-1-5-18" ), users = sid_of( "S-1-5-32-545" );
    HANDLE token;
    ACCESS_MASK mask;
    DWORD size, ret;
    PACL acl;

    setvbuf( stdout, NULL, _IONBF, 0 );
    OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token );
    GetTokenInformation( token, TokenUser, user, sizeof(user_buf), &size );
    CloseHandle( token );
    me = user->User.Sid;

    acl = new_acl();
    mask = rights( acl, me, &ret );
    CHECK( !ret && mask == 0, "empty ACL: no rights (%lu, %#lx)", ret, mask );

    acl = new_acl();
    AddAccessAllowedAce( acl, ACL_REVISION, FILE_GENERIC_READ, everyone );
    mask = rights( acl, me, &ret );
    CHECK( !ret && mask == FILE_GENERIC_READ, "allowed to Everyone: the user gets it (%#lx)", mask );
    mask = rights( acl, system, &ret );
    CHECK( !ret && mask == FILE_GENERIC_READ, "and so does another account (%#lx)", mask );

    acl = new_acl();
    AddAccessDeniedAce( acl, ACL_REVISION, FILE_WRITE_DATA, me );
    AddAccessAllowedAce( acl, ACL_REVISION, FILE_ALL_ACCESS, everyone );
    mask = rights( acl, me, &ret );
    CHECK( !ret && mask == (FILE_ALL_ACCESS & ~FILE_WRITE_DATA), "a deny before an allow removes the right (%#lx)", mask );
    mask = rights( acl, system, &ret );
    CHECK( !ret && mask == FILE_ALL_ACCESS, "the deny does not apply to another account (%#lx)", mask );

    acl = new_acl();
    AddAccessAllowedAce( acl, ACL_REVISION, FILE_ALL_ACCESS, me );
    AddAccessDeniedAce( acl, ACL_REVISION, FILE_WRITE_DATA, me );
    mask = rights( acl, me, &ret );
    CHECK( !ret && mask == FILE_ALL_ACCESS, "an allow before a deny wins (%#lx)", mask );

    acl = new_acl();
    AddAccessAllowedAce( acl, ACL_REVISION, FILE_GENERIC_EXECUTE, users );
    mask = rights( acl, me, &ret );
    CHECK( !ret && mask == FILE_GENERIC_EXECUTE, "an ACE for BUILTIN\\Users applies to its member (%#lx)", mask );

    acl = new_acl();
    AddAccessAllowedAceEx( acl, ACL_REVISION, INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE, FILE_ALL_ACCESS, me );
    AddAccessAllowedAce( acl, ACL_REVISION, FILE_GENERIC_READ, me );
    mask = rights( acl, me, &ret );
    CHECK( !ret && mask == FILE_GENERIC_READ, "an inherit-only ACE does not apply (%#lx)", mask );

    {
        TRUSTEE_A trusteeA;
        acl = new_acl();
        AddAccessAllowedAce( acl, ACL_REVISION, FILE_GENERIC_READ, me );
        BuildTrusteeWithNameA( &trusteeA, (char *)"CURRENT_USER" );
        mask = 0xdeadbeef;
        ret = GetEffectiveRightsFromAclA( acl, &trusteeA, &mask );
        CHECK( !ret && mask == FILE_GENERIC_READ, "the A version with CURRENT_USER (%lu, %#lx)", ret, mask );
    }

    LocalFree( everyone ); LocalFree( system ); LocalFree( users );
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
