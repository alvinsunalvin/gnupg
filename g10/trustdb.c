/* trustdb.c
 *	Copyright (C) 1998 Free Software Foundation, Inc.
 *
 * This file is part of GNUPG.
 *
 * GNUPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GNUPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "errors.h"
#include "iobuf.h"
#include "keydb.h"
#include "memory.h"
#include "util.h"
#include "trustdb.h"
#include "options.h"
#include "packet.h"
#include "main.h"
#include "i18n.h"
#include "tdbio.h"


#if MAX_FINGERPRINT_LEN > 20
  #error Must change structure of trustdb
#endif

typedef struct local_id_info *LOCAL_ID_INFO;
struct local_id_info {
    LOCAL_ID_INFO next;
    ulong lid;
    unsigned flag;
};


typedef struct trust_info TRUST_INFO;
struct trust_info {
    ulong    lid;
    unsigned trust;
};


typedef struct trust_seg_list *TRUST_SEG_LIST;
struct trust_seg_list {
    TRUST_SEG_LIST next;
    int   nseg;     /* number of segments */
    int   dup;
    TRUST_INFO seg[1];	 /* segment list */
};



struct recno_list_struct {
    struct recno_list_struct *next;
    ulong recno;
    int type;
};
typedef struct recno_list_struct *RECNO_LIST;


static int walk_sigrecs( SIGREC_CONTEXT *c );

static LOCAL_ID_INFO *new_lid_table(void);
static void release_lid_table( LOCAL_ID_INFO *tbl );
static int ins_lid_table_item( LOCAL_ID_INFO *tbl, ulong lid, unsigned flag );
static int qry_lid_table_flag( LOCAL_ID_INFO *tbl, ulong lid, unsigned *flag );

static void print_user_id( const char *text, u32 *keyid );
static int list_sigs( ulong pubkey_id );
static int do_check( TRUSTREC *drec, unsigned *trustlevel );
static int get_dir_record( PKT_public_key *pk, TRUSTREC *rec );


/* a table used to keep track of ultimately trusted keys
 * which are the ones from our secrings */
static LOCAL_ID_INFO *ultikey_table;



#define HEXTOBIN(a) ( (a) >= '0' && (a) <= '9' ? ((a)-'0') : \
		      (a) >= 'A' && (a) <= 'F' ? ((a)-'A'+10) : ((a)-'a'+10))



/**********************************************
 ***********  record read write  **************
 **********************************************/

static void
die_invalid_db()
{
    log_error(_(
	"The trust DB is corrupted; please run \"gpgm --fix-trust-db\".\n") );
    g10_exit(2);
}

/****************
 * Read a record but die if it does not exist
 */
static void
read_record( ulong recno, TRUSTREC *rec, int rectype )
{
    int rc = tdbio_read_record( recno, rec, rectype );
    if( !rc )
	return;
    log_error("trust record %lu, req type %d: read failed: %s\n",
				    recno, rectype,  g10_errstr(rc) );
    die_invalid_db();
}


/****************
 * Wirte a record but die on error
 */
static void
write_record( TRUSTREC *rec )
{
    int rc = tdbio_write_record( rec );
    if( !rc )
	return;
    log_error("trust record %lu, type %d: write failed: %s\n",
			    rec->recnum, rec->rectype, g10_errstr(rc) );
    die_invalid_db();
}

/****************
 * Delete a record but die on error
 */
static void
delete_record( ulong recno )
{
    int rc = tdbio_delete_record( recno );
    if( !rc )
	return;
    log_error("trust record %lu: delete failed: %s\n",
					      recno, g10_errstr(rc) );
    die_invalid_db();
}

/****************
 * sync the db
 */
static void
do_sync( )
{
    int rc = tdbio_sync();
    if( !rc )
	return;
    log_error("trust db: sync failed: %s\n", g10_errstr(rc) );
    g10_exit(2);
}



/**********************************************
 ************* list helpers *******************
 **********************************************/

/****************
 * Insert a new item into a recno list
 */
static void
ins_recno_list( RECNO_LIST *head, ulong recno, int type )
{
    RECNO_LIST item = m_alloc( sizeof *item );

    item->recno = recno;
    item->type = type;
    item->next = *head;
    *head = item;
}

static RECNO_LIST
qry_recno_list( RECNO_LIST list, ulong recno, int type	)
{
    for( ; list; list = list->next ) {
	if( list->recno == recno && (!type || list->type == type) )
	    return list;
    }
    return NULL;
}


static void
rel_recno_list( RECNO_LIST *head )
{
    RECNO_LIST r, r2;

    for(r = *head; r; r = r2 ) {
	r2 = r->next;
	m_free(r);
    }
    *head = NULL;
}

static LOCAL_ID_INFO *
new_lid_table(void)
{
    return m_alloc_clear( 16 * sizeof(LOCAL_ID_INFO));
}

static void
release_lid_table( LOCAL_ID_INFO *tbl )
{
    LOCAL_ID_INFO a, a2;
    int i;

    for(i=0; i < 16; i++ ) {
	for(a=tbl[i]; a; a = a2 ) {
	    a2 = a->next;
	    m_free(a);
	}
    }
    m_free(tbl);
}

/****************
 * Add a new item to the table or return 1 if we already have this item
 * fixme: maybe it's a good idea to take items from an unused item list.
 */
static int
ins_lid_table_item( LOCAL_ID_INFO *tbl, ulong lid, unsigned flag )
{
    LOCAL_ID_INFO a;

    for( a = tbl[lid & 0x0f]; a; a = a->next )
	if( a->lid == lid )
	    return 1;
    a = m_alloc( sizeof *a );
    a->lid = lid;
    a->flag = flag;
    a->next = tbl[lid & 0x0f];
    tbl[lid & 0x0f] = a;
    return 0;
}

static int
qry_lid_table_flag( LOCAL_ID_INFO *tbl, ulong lid, unsigned *flag )
{
    LOCAL_ID_INFO a;

    for( a = tbl[lid & 0x0f]; a; a = a->next )
	if( a->lid == lid ) {
	    if( flag )
		*flag = a->flag;
	    return 0;
	}
    return -1;
}



/****************
 * Return the keyid from the primary key identified by LID.
 */
int
keyid_from_lid( ulong lid, u32 *keyid )
{
    TRUSTREC rec;
    int rc;

    rc = tdbio_read_record( lid, &rec, 0 );
    if( rc ) {
	log_error("error reading dir record for LID %lu: %s\n",
						    lid, g10_errstr(rc));
	return G10ERR_TRUSTDB;
    }
    if( rec.rectype == RECTYPE_SDIR )
	return 0;
    if( rec.rectype != RECTYPE_DIR ) {
	log_error("lid %lu: expected dir record, got type %d\n",
						    lid, rec.rectype );
	return G10ERR_TRUSTDB;
    }
    if( !rec.r.dir.keylist ) {
	log_error("no primary key for LID %lu\n", lid );
	return G10ERR_TRUSTDB;
    }
    rc = tdbio_read_record( rec.r.dir.keylist, &rec, RECTYPE_KEY );
    if( rc ) {
	log_error("error reading primary key for LID %lu: %s\n",
						    lid, g10_errstr(rc));
	return G10ERR_TRUSTDB;
    }
    keyid_from_fingerprint( rec.r.key.fingerprint, rec.r.key.fingerprint_len,
			    keyid );

    return 0;
}


ulong
lid_from_keyblock( KBNODE keyblock )
{
    KBNODE node = find_kbnode( keyblock, PKT_PUBLIC_KEY );
    PKT_public_key *pk;
    if( !node )
	BUG();
    pk = node->pkt->pkt.public_key;
    if( !pk->local_id ) {
	TRUSTREC rec;

	get_dir_record( pk, &rec );
    }
    return pk->local_id;
}



/****************
 * Walk through the signatures of a public key.
 * The caller must provide a context structure, with all fields set
 * to zero, but the local_id field set to the requested key;
 * This function does not change this field.  On return the context
 * is filled with the local-id of the signature and the signature flag.
 * No fields should be changed (clearing all fields and setting
 * pubkeyid is okay to continue with an other pubkey)
 * Returns: 0 - okay, -1 for eof (no more sigs) or any other errorcode
 */
static int
walk_sigrecs( SIGREC_CONTEXT *c )
{
    TRUSTREC *r;
    ulong rnum;

    if( c->ctl.eof )
	return -1;
    r = &c->ctl.rec;
    if( !c->ctl.init_done ) {
	c->ctl.init_done = 1;
	read_record( c->lid, r, 0 );
	if( r->rectype != RECTYPE_DIR ) {
	    c->ctl.eof = 1;
	    return -1;	/* return eof */
	}
	c->ctl.nextuid = r->r.dir.uidlist;
	/* force a read */
	c->ctl.index = SIGS_PER_RECORD;
	r->r.sig.next = 0;
    }

    /* need a loop to skip over deleted sigs */
    do {
	if( c->ctl.index >= SIGS_PER_RECORD ) { /* read the record */
	    rnum = r->r.sig.next;
	    if( !rnum && c->ctl.nextuid ) { /* read next uid record */
		read_record( c->ctl.nextuid, r, RECTYPE_UID );
		c->ctl.nextuid = r->r.uid.next;
		rnum = r->r.uid.siglist;
	    }
	    if( !rnum ) {
		c->ctl.eof = 1;
		return -1;  /* return eof */
	    }
	    read_record( rnum, r, RECTYPE_SIG );
	    if( r->r.sig.lid != c->lid ) {
		log_error(_("chained sigrec %lu has a wrong owner\n"), rnum );
		c->ctl.eof = 1;
		die_invalid_db();
	    }
	    c->ctl.index = 0;
	}
    } while( !r->r.sig.sig[c->ctl.index++].lid );

    c->sig_lid = r->r.sig.sig[c->ctl.index-1].lid;
    c->sig_flag = r->r.sig.sig[c->ctl.index-1].flag;
    return 0;
}




/***********************************************
 *************	Trust  stuff  ******************
 ***********************************************/


/****************
 * Verify that all our public keys are in the trustDB.
 */
static int
verify_own_keys()
{
    int rc;
    void *enum_context = NULL;
    PKT_secret_key *sk = m_alloc_clear( sizeof *sk );
    PKT_public_key *pk = m_alloc_clear( sizeof *pk );
    u32 keyid[2];

    while( !(rc=enum_secret_keys( &enum_context, sk, 0 ) ) ) {
	keyid_from_sk( sk, keyid );

	if( DBG_TRUST )
	    log_debug("key %08lX: checking secret key\n", (ulong)keyid[1] );

	if( is_secret_key_protected( sk ) < 1 )
	    log_info("note: secret key %08lX is NOT protected.\n",
							    (ulong)keyid[1] );

	/* see whether we can access the public key of this secret key */
	memset( pk, 0, sizeof *pk );
	rc = get_pubkey( pk, keyid );
	if( rc ) {
	    log_info(_("key %08lX: secret key without public key - skipped\n"),
							    (ulong)keyid[1] );
	    goto skip;
	}

	if( cmp_public_secret_key( pk, sk ) ) {
	    log_info(_("key %08lX: secret and public key don't match\n"),
							    (ulong)keyid[1] );
	    goto skip;
	}

	/* make sure that the pubkey is in the trustdb */
	rc = query_trust_record( pk );
	if( rc == -1 ) { /* put it into the trustdb */
	    rc = insert_trust_record( pk );
	    if( rc ) {
		log_error(_("key %08lX: can't put it into the trustdb\n"),
							    (ulong)keyid[1] );
		goto skip;
	    }
	}
	else if( rc ) {
	    log_error(_("key %08lX: query record failed\n"), (ulong)keyid[1] );
	    goto skip;

	}

	if( DBG_TRUST )
	    log_debug("key %08lX.%lu: stored into ultikey_table\n",
				    (ulong)keyid[1], pk->local_id );
	if( ins_lid_table_item( ultikey_table, pk->local_id, 0 ) )
	    log_error(_("key %08lX: already in secret key table\n"),
							(ulong)keyid[1]);
	else if( opt.verbose )
	    log_info(_("key %08lX: accepted as secret key.\n"),
							(ulong)keyid[1]);
      skip:
	release_secret_key_parts( sk );
	release_public_key_parts( pk );
    }
    if( rc != -1 )
	log_error(_("enum_secret_keys failed: %s\n"), g10_errstr(rc) );
    else
	rc = 0;

    enum_secret_keys( &enum_context, NULL, 0 ); /* free context */
    free_secret_key( sk );
    free_public_key( pk );
    return rc;
}


static void
print_user_id( const char *text, u32 *keyid )
{
    char *p;
    size_t n;

    p = get_user_id( keyid, &n );
    if( *text ) {
	fputs( text, stdout);
	putchar(' ');
    }
    putchar('\"');
    print_string( stdout, p, n, 0 );
    putchar('\"');
    putchar('\n');
    m_free(p);
}


static int
print_keyid( FILE *fp, ulong lid )
{
    u32 ki[2];
    if( keyid_from_lid( lid, ki ) )
	return fprintf(fp, "????????.%lu", lid );
    else
	return fprintf(fp, "%08lX.%lu", (ulong)ki[1], lid );
}

static int
print_trust( FILE *fp, unsigned trust )
{
    int c;
    switch( trust ) {
      case TRUST_UNKNOWN:   c = 'o'; break;
      case TRUST_EXPIRED:   c = 'e'; break;
      case TRUST_UNDEFINED: c = 'q'; break;
      case TRUST_NEVER:     c = 'n'; break;
      case TRUST_MARGINAL:  c = 'm'; break;
      case TRUST_FULLY:     c = 'f'; break;
      case TRUST_ULTIMATE:  c = 'u'; break;
      default: fprintf(fp, "%02x", trust ); return 2;
    }
    putc(c, fp);
    return 1;
}


static int
print_sigflags( FILE *fp, unsigned flags )
{
    if( flags & SIGF_CHECKED ) {
	fprintf(fp,"%c%c%c",
	   (flags & SIGF_VALID)   ? 'V':'-',
	   (flags & SIGF_EXPIRED) ? 'E':'-',
	   (flags & SIGF_REVOKED) ? 'R':'-');
    }
    else if( flags & SIGF_NOPUBKEY)
	fputs("?--", fp);
    else
	fputs("---", fp);
    return 3;
}

/* (a non-recursive algorithm would be easier) */
static int
do_list_sigs( ulong root, ulong pk_lid, int depth,
	      LOCAL_ID_INFO *lids, unsigned *lineno )
{
    SIGREC_CONTEXT sx;
    int rc;
    u32 keyid[2];

    memset( &sx, 0, sizeof sx );
    sx.lid = pk_lid;
    for(;;) {
	rc = walk_sigrecs( &sx );
	if( rc )
	    break;
	rc = keyid_from_lid( sx.sig_lid, keyid );
	if( rc ) {
	    printf("%6u: %*s????????.%lu:", *lineno, depth*4, "", sx.sig_lid );
	    print_sigflags( stdout, sx.sig_flag );
	    putchar('\n');
	    ++*lineno;
	}
	else {
	    printf("%6u: %*s%08lX.%lu:", *lineno, depth*4, "",
			      (ulong)keyid[1], sx.sig_lid );
	    print_sigflags( stdout, sx.sig_flag );
	    putchar(' ');
	    /* check whether we already checked this pk_lid */
	    if( !qry_lid_table_flag( ultikey_table, sx.sig_lid, NULL ) ) {
		print_user_id("[ultimately trusted]", keyid);
		++*lineno;
	    }
	    else if( sx.sig_lid == pk_lid ) {
		printf("[self-signature]\n");
		++*lineno;
	    }
	    else if( sx.sig_lid == root ) {
		printf("[closed]\n");
		++*lineno;
	    }
	    else if( ins_lid_table_item( lids, sx.sig_lid, *lineno ) ) {
		unsigned refline;
		qry_lid_table_flag( lids, sx.sig_lid, &refline );
		printf("[see line %u]\n", refline);
		++*lineno;
	    }
	    else if( depth+1 >= MAX_LIST_SIGS_DEPTH  ) {
		print_user_id( "[too deeply nested]", keyid );
		++*lineno;
	    }
	    else {
		print_user_id( "", keyid );
		++*lineno;
		rc = do_list_sigs( root, sx.sig_lid, depth+1, lids, lineno );
		if( rc )
		    break;
	    }
	}
    }
    return rc==-1? 0 : rc;
}

/****************
 * List all signatures of a public key
 */
static int
list_sigs( ulong pubkey_id )
{
    int rc;
    u32 keyid[2];
    LOCAL_ID_INFO *lids;
    unsigned lineno = 1;

    rc = keyid_from_lid( pubkey_id, keyid );
    if( rc )
	return rc;
    printf("Signatures of %08lX.%lu ", (ulong)keyid[1], pubkey_id );
    print_user_id("", keyid);
    printf("----------------------\n");

    lids = new_lid_table();
    rc = do_list_sigs( pubkey_id, pubkey_id, 0, lids, &lineno );
    putchar('\n');
    release_lid_table(lids);
    return rc;
}

/****************
 * List all records of a public key
 */
static int
list_records( ulong lid )
{
    int rc;
    TRUSTREC dr, ur, rec;
    ulong recno;

    rc = tdbio_read_record( lid, &dr, RECTYPE_DIR );
    if( rc ) {
	log_error("lid %lu: read dir record failed: %s\n", lid, g10_errstr(rc));
	return rc;
    }
    tdbio_dump_record( &dr, stdout );

    for( recno=dr.r.dir.keylist; recno; recno = rec.r.key.next ) {
	rc = tdbio_read_record( recno, &rec, 0 );
	if( rc ) {
	    log_error("lid %lu: read key record failed: %s\n",
						lid, g10_errstr(rc));
	    return rc;
	}
	tdbio_dump_record( &rec, stdout );
    }

    for( recno=dr.r.dir.uidlist; recno; recno = ur.r.uid.next ) {
	rc = tdbio_read_record( recno, &ur, RECTYPE_UID );
	if( rc ) {
	    log_error("lid %lu: read uid record failed: %s\n",
						lid, g10_errstr(rc));
	    return rc;
	}
	tdbio_dump_record( &ur, stdout );
	/* preference records */
	for(recno=ur.r.uid.prefrec; recno; recno = rec.r.pref.next ) {
	    rc = tdbio_read_record( recno, &rec, RECTYPE_PREF );
	    if( rc ) {
		log_error("lid %lu: read pref record failed: %s\n",
						    lid, g10_errstr(rc));
		return rc;
	    }
	    tdbio_dump_record( &rec, stdout );
	}
	/* sig records */
	for(recno=ur.r.uid.siglist; recno; recno = rec.r.sig.next ) {
	    rc = tdbio_read_record( recno, &rec, RECTYPE_SIG );
	    if( rc ) {
		log_error("lid %lu: read sig record failed: %s\n",
						    lid, g10_errstr(rc));
		return rc;
	    }
	    tdbio_dump_record( &rec, stdout );
	}
    }

    /* add cache record dump here */



    return rc;
}



/****************
 * Given the directory record of a key, check whether we can
 * find a path to an ultimately trusted key.  We do this by
 * checking all key signatures up to a some depth.
 */
static int
verify_key( int depth, int max_depth, TRUSTREC *drec )
{
    ulong rn, uidrn;
    int marginal=0;
    int fully=0;
    size_t dbglen;

    /* Note: If used stack size is an issue, we could reuse the
     * trustrec vars and reread them as needed */

    dbglen = printf("verify_key: depth=%d %*s", depth, depth*3,"" );
    dbglen += print_keyid( stdout, drec->recnum );
    dbglen += printf(" ot=");
    dbglen += print_trust(stdout, drec->r.dir.ownertrust );
    dbglen += printf(" -> ");

    if( depth >= max_depth ) {
	/* max cert_depth reached */
	puts("undefined (too deep)");
	return TRUST_UNDEFINED;
    }
    if( !qry_lid_table_flag( ultikey_table, drec->r.dir.lid, NULL ) ) {
	/* we are at the end of a path */
	puts("ultimate");
	return TRUST_ULTIMATE;
    }

    /* loop over all user-ids */
    for( rn = drec->r.dir.uidlist; rn; rn = uidrn ) {
	TRUSTREC rec;  /* used for uids and sigs */
	ulong sigrn;

	read_record( rn, &rec, RECTYPE_UID );
	uidrn = rec.r.uid.next;
	/* fixme: continue if the uidrec is not marked valid */

	/* loop over all signature records */
	for( rn = rec.r.uid.siglist; rn; rn = sigrn ) {
	    int i;

	    read_record( rn, &rec, RECTYPE_SIG );
	    sigrn = rec.r.sig.next;

	    for(i=0; i < SIGS_PER_RECORD; i++ ) {
		TRUSTREC tmp;
		int ot, nt;

		if( !rec.r.sig.sig[i].lid )
		    continue; /* skip deleted sigs */
		if( !(rec.r.sig.sig[i].flag & SIGF_CHECKED) )
		    continue; /* skip unchecked signatures */
		if( !(rec.r.sig.sig[i].flag & SIGF_VALID) )
		    continue; /* skip invalid signatures */
		if( (rec.r.sig.sig[i].flag & SIGF_EXPIRED) )
		    continue; /* skip expired signatures */
		if( (rec.r.sig.sig[i].flag & SIGF_REVOKED) )
		    continue; /* skip revoked signatures */
		/* fixme: skip duplicates */

		read_record( rec.r.sig.sig[i].lid, &tmp, RECTYPE_DIR );
		ot = tmp.r.dir.ownertrust & TRUST_MASK;
	      #if 0 /* Does not work, because the owner trust of our
		     * own keys is not always set
		     * -- fix this in verify_own_keys() ? */
		if( ot < TRUST_MARGINAL ) {
		    printf(". ");
		    continue;  /* ownertrust is too low; don't need to check */
		}
	      #endif
		if( ot >= TRUST_FULLY )
		    ot = TRUST_FULLY;  /* just in case */

		puts("");
		nt = verify_key( depth+1, max_depth, &tmp ) & TRUST_MASK;
		if( nt < TRUST_MARGINAL ) {
		    printf("%*s* ", dbglen, "");
		    dbglen += 2;
		    continue;
		}

		if( nt == TRUST_ULTIMATE ) {
		    /* we have signed this key and only in this special case
		     * we assume a completes-needed or marginals-needed of 1 */
		    printf("%*s", dbglen, "");
		    if( ot == TRUST_MARGINAL )
			puts("marginal (1st level)");
		    else if( ot == TRUST_FULLY )
			puts("fully    (1st level)");
		    else
			puts("?????    (1st level)");
		    return ot;
		}

		if( nt >= TRUST_FULLY )
		    fully++;
		if( nt >= TRUST_MARGINAL )
		    marginal++;

		if( fully >= opt.completes_needed
		    || marginal >= opt.marginals_needed ) {
		    printf("%*s", dbglen, "");
		    puts("fully");
		    return TRUST_FULLY;
		}
	    }
	}
    }
    printf("%*s", dbglen, "");
    if( marginal ) {
	puts("marginal");
	return TRUST_MARGINAL;
    }
    puts("undefined");
    return TRUST_UNDEFINED;
}


static int
list_paths( int depth, int max_depth, TRUSTREC *drec )
{
    ulong rn, uidrn;
    int marginal=0;
    int fully=0;
    size_t dbglen;

    if( depth >= max_depth ) {
	/* max cert_depth reached */
	puts("undefined (too deep)");
	return TRUST_UNDEFINED;
    }
    if( !qry_lid_table_flag( ultikey_table, drec->r.dir.lid, NULL ) ) {
	/* we are at the end of a path */
	puts("ultimate");
	return TRUST_ULTIMATE;
    }

    /* loop over all user-ids */
    for( rn = drec->r.dir.uidlist; rn; rn = uidrn ) {
	TRUSTREC rec;  /* used for uids and sigs */
	ulong sigrn;

	read_record( rn, &rec, RECTYPE_UID );
	uidrn = rec.r.uid.next;
	/* fixme: continue if the uidrec is not marked valid */

	/* loop over all signature records */
	for( rn = rec.r.uid.siglist; rn; rn = sigrn ) {
	    int i;

	    read_record( rn, &rec, RECTYPE_SIG );
	    sigrn = rec.r.sig.next;

	    for(i=0; i < SIGS_PER_RECORD; i++ ) {
		TRUSTREC tmp;
		int ot, nt;

		if( !rec.r.sig.sig[i].lid )
		    continue; /* skip deleted sigs */
		if( !(rec.r.sig.sig[i].flag & SIGF_CHECKED) )
		    continue; /* skip unchecked signatures */
		if( !(rec.r.sig.sig[i].flag & SIGF_VALID) )
		    continue; /* skip invalid signatures */
		if( (rec.r.sig.sig[i].flag & SIGF_EXPIRED) )
		    continue; /* skip expired signatures */
		if( (rec.r.sig.sig[i].flag & SIGF_REVOKED) )
		    continue; /* skip revoked signatures */
		/* fixme: skip duplicates */

		read_record( rec.r.sig.sig[i].lid, &tmp, RECTYPE_DIR );
		ot = tmp.r.dir.ownertrust & TRUST_MASK;
		if( ot < TRUST_MARGINAL ) {
		    printf(". ");
		    continue;  /* ownertrust is too low; don't need to check */
		}

		if( ot >= TRUST_FULLY )
		    ot = TRUST_FULLY;  /* just in case */

		puts("");
		nt = verify_key( depth+1, max_depth, &tmp ) & TRUST_MASK;
		if( nt < TRUST_MARGINAL ) {
		    printf("%*s* ", dbglen, "");
		    dbglen += 2;
		    continue;
		}

		if( nt == TRUST_ULTIMATE ) {
		    /* we have signed this key and only in this special case
		     * we assume a completes-needed or marginals-needed of 1 */
		    printf("%*s", dbglen, "");
		    if( ot == TRUST_MARGINAL )
			puts("marginal (1st level)");
		    else if( ot == TRUST_FULLY )
			puts("fully    (1st level)");
		    else
			puts("?????    (1st level)");
		    return ot;
		}

		if( nt >= TRUST_FULLY )
		    fully++;
		if( nt >= TRUST_MARGINAL )
		    marginal++;

		if( fully >= opt.completes_needed
		    || marginal >= opt.marginals_needed ) {
		    printf("%*s", dbglen, "");
		    puts("fully");
		    return TRUST_FULLY;
		}
	    }
	}
    }
    printf("%*s", dbglen, "");
    if( marginal ) {
	puts("marginal");
	return TRUST_MARGINAL;
    }
    puts("undefined");
    return TRUST_UNDEFINED;
}


/****************
 * we have the pubkey record and all needed informations are in the trustdb
 * but nothing more is known.
 */
static int
do_check( TRUSTREC *dr, unsigned *trustlevel )
{
    if( !dr->r.dir.keylist ) {
	log_error("Ooops, no keys\n");
	return G10ERR_TRUSTDB;
    }
    if( !dr->r.dir.uidlist ) {
	log_error("Ooops, no user ids\n");
	return G10ERR_TRUSTDB;
    }

    *trustlevel = verify_key( 1, 5, dr );

    if( dr->r.dir.dirflags & DIRF_REVOKED )
	*trustlevel |= TRUST_FLAG_REVOKED;

    return 0;
}


/****************
 * Perform some checks over the trustdb
 *  level 0: only open the db
 *	  1: used for initial program startup
 */
int
init_trustdb( int level, const char *dbname )
{
    int rc=0;

    if( !ultikey_table )
	ultikey_table = new_lid_table();

    if( !level || level==1 ) {
	rc = tdbio_set_dbname( dbname, !!level );
	if( rc )
	    return rc;
	if( !level )
	    return 0;

	/* verify that our own keys are in the trustDB
	 * or move them to the trustdb. */
	rc = verify_own_keys();

	/* should we check whether there is no other ultimately trusted
	 * key in the database? */

    }
    else
	BUG();

    return rc;
}


void
list_trustdb( const char *username )
{
    TRUSTREC rec;

    if( username && *username == '#' ) {
	int rc;
	ulong lid = atoi(username+1);

	if( (rc = list_records( lid)) )
	    log_error("user '%s' read problem: %s\n", username, g10_errstr(rc));
	else if( (rc = list_sigs( lid )) )
	    log_error("user '%s' list problem: %s\n", username, g10_errstr(rc));
    }
    else if( username ) {
	PKT_public_key *pk = m_alloc_clear( sizeof *pk );
	int rc;

	if( (rc = get_pubkey_byname( pk, username )) )
	    log_error("user '%s' not found: %s\n", username, g10_errstr(rc) );
	else if( (rc=tdbio_search_dir_bypk( pk, &rec )) && rc != -1 )
	    log_error("problem finding '%s' in trustdb: %s\n",
						username, g10_errstr(rc));
	else if( rc == -1 )
	    log_error("user '%s' not in trustdb\n", username);
	else if( (rc = list_records( pk->local_id)) )
	    log_error("user '%s' read problem: %s\n", username, g10_errstr(rc));
	else if( (rc = list_sigs( pk->local_id )) )
	    log_error("user '%s' list problem: %s\n", username, g10_errstr(rc));
	free_public_key( pk );
    }
    else {
	ulong recnum;
	int i;

	printf("TrustDB: %s\n", tdbio_get_dbname() );
	for(i=9+strlen(tdbio_get_dbname()); i > 0; i-- )
	    putchar('-');
	putchar('\n');
	for(recnum=0; !tdbio_read_record( recnum, &rec, 0); recnum++ )
	    tdbio_dump_record( &rec, stdout );
    }
}

/****************
 * Print a list of all defined owner trust value.
 */
void
export_ownertrust()
{
    TRUSTREC rec;
    TRUSTREC rec2;
    ulong recnum;
    int i;
    byte *p;
    int rc;

    printf("# List of assigned trustvalues, created %s\n"
	   "# (Use \"gpgm --import-ownertrust\" to restore them)\n",
	   asctimestamp( make_timestamp() ) );
    for(recnum=0; !tdbio_read_record( recnum, &rec, 0); recnum++ ) {
	if( rec.rectype == RECTYPE_DIR ) {
	    if( !rec.r.dir.keylist ) {
		log_error("Oops; directory record w/o primary key\n");
		continue;
	    }
	    if( !rec.r.dir.ownertrust )
		continue;
	    rc = tdbio_read_record( rec.r.dir.keylist, &rec2, RECTYPE_KEY);
	    if( rc ) {
		log_error("error reading key record: %s\n", g10_errstr(rc));
		continue;
	    }
	    p = rec2.r.key.fingerprint;
	    for(i=0; i < rec2.r.key.fingerprint_len; i++, p++ )
		printf("%02X", *p );
	    printf(":%u:\n", (unsigned)rec.r.dir.ownertrust );
	}
    }
}


void
import_ownertrust( const char *fname )
{
    FILE *fp;
    int is_stdin=0;
    char line[256];
    char *p;
    size_t n, fprlen;
    unsigned otrust;

    if( !fname || (*fname == '-' && !fname[1]) ) {
	fp = stdin;
	fname = "[stdin]";
	is_stdin = 1;
    }
    else if( !(fp = fopen( fname, "r" )) ) {
	log_error_f(fname, _("can't open file: %s\n"), strerror(errno) );
	return;
    }

    while( fgets( line, DIM(line)-1, fp ) ) {
	TRUSTREC rec;
	int rc;

	if( !*line || *line == '#' )
	    continue;
	n = strlen(line);
	if( line[n-1] != '\n' ) {
	    log_error_f(fname, "line to long\n" );
	    break; /* can't continue */
	}
	for(p = line; *p && *p != ':' ; p++ )
	    if( !isxdigit(*p) )
		break;
	if( *p != ':' ) {
	    log_error_f(fname, "error: missing colon\n" );
	    continue;
	}
	fprlen = p - line;
	if( fprlen != 32 && fprlen != 40 ) {
	    log_error_f(fname, "error: invalid fingerprint\n" );
	    continue;
	}
	if( sscanf(p, ":%u:", &otrust ) != 1 ) {
	    log_error_f(fname, "error: no otrust value\n" );
	    continue;
	}
	if( !otrust )
	    continue; /* no otrust defined - no need to update or insert */
	/* convert the ascii fingerprint to binary */
	for(p=line, fprlen=0; *p != ':'; p += 2 )
	    line[fprlen++] = HEXTOBIN(p[0]) * 16 + HEXTOBIN(p[1]);
	line[fprlen] = 0;

      repeat:
	rc = tdbio_search_dir_byfpr( line, fprlen, 0, &rec );
	if( !rc ) { /* found: update */
	    if( rec.r.dir.ownertrust )
		log_info("LID %lu: changing trust from %u to %u\n",
			  rec.r.dir.lid, rec.r.dir.ownertrust, otrust );
	    else
		log_info("LID %lu: setting trust to %u\n",
				   rec.r.dir.lid, otrust );
	    rec.r.dir.ownertrust = otrust;
	    write_record( &rec );
	}
	else if( rc == -1 ) { /* not found; get the key from the ring */
	    PKT_public_key *pk = m_alloc_clear( sizeof *pk );

	    log_info_f(fname, "key not in trustdb, searching ring.\n");
	    rc = get_pubkey_byfprint( pk, line, fprlen );
	    if( rc )
		log_info_f(fname, "key not in ring: %s\n", g10_errstr(rc));
	    else {
		rc = query_trust_record( pk );	/* only as assertion */
		if( rc != -1 )
		    log_error_f(fname, "Oops: key is now in trustdb???\n");
		else {
		    rc = insert_trust_record( pk );
		    if( !rc )
			goto repeat; /* update the ownertrust */
		    log_error_f(fname, "insert trust record failed: %s\n",
							   g10_errstr(rc) );
		}
	    }
	}
	else /* error */
	    log_error_f(fname, "error finding dir record: %s\n",
						    g10_errstr(rc));
    }
    if( ferror(fp) )
	log_error_f(fname, _("read error: %s\n"), strerror(errno) );
    if( !is_stdin )
	fclose(fp);
    do_sync();
}


void
list_trust_path( int max_depth, const char *username )
{
    int rc;
    int wipe=0;
    TRUSTREC rec;
    PKT_public_key *pk = m_alloc_clear( sizeof *pk );

    if( max_depth < 0 ) {
	wipe = 1;
	max_depth = -max_depth;
    }
    if( max_depth < 1 )
	max_depth = 1;

    if( (rc = get_pubkey_byname( pk, username )) )
	log_error("user '%s' not found: %s\n", username, g10_errstr(rc) );
    else if( (rc=tdbio_search_dir_bypk( pk, &rec )) && rc != -1 )
	log_error("problem finding '%s' in trustdb: %s\n",
					    username, g10_errstr(rc));
    else if( rc == -1 ) {
	log_info("user '%s' not in trustdb - inserting\n", username);
	rc = insert_trust_record( pk );
	if( rc )
	    log_error("failed to put '%s' into trustdb: %s\n", username, g10_errstr(rc));
	else {
	    assert( pk->local_id );
	}
    }


    free_public_key( pk );
}


/****************
 * Check the complete trustdb or only the entries for the given username.
 */
void
check_trustdb( const char *username )
{
    TRUSTREC rec;
    KBNODE keyblock = NULL;
    KBPOS kbpos;
    int rc;

    if( username ) {
	rc = find_keyblock_byname( &kbpos, username );
	if( !rc )
	    rc = read_keyblock( &kbpos, &keyblock );
	if( rc ) {
	    log_error("%s: keyblock read problem: %s\n",
				    username, g10_errstr(rc));
	}
	else {
	    int modified;

	    rc = update_trust_record( keyblock, &modified );
	    if( rc == -1 ) { /* not yet in trustdb: insert */
		rc = insert_trust_record(
			    find_kbnode( keyblock, PKT_PUBLIC_KEY
				       ) ->pkt->pkt.public_key );

	    }
	    if( rc )
		log_error("%s: update failed: %s\n",
					   username, g10_errstr(rc) );
	    else if( modified )
		log_info("%s: updated\n", username );
	    else
		log_info("%s: okay\n", username );

	}
	release_kbnode( keyblock ); keyblock = NULL;
    }
    else {
	ulong recnum;
	ulong count=0, upd_count=0, err_count=0, skip_count=0;

	for(recnum=0; !tdbio_read_record( recnum, &rec, 0); recnum++ ) {
	    if( rec.rectype == RECTYPE_DIR ) {
		TRUSTREC tmp;
		int modified;

		if( !rec.r.dir.keylist ) {
		    log_info("lid %lu: dir record w/o key - skipped\n", recnum);
		    count++;
		    skip_count++;
		    continue;
		}

		read_record( rec.r.dir.keylist, &tmp, RECTYPE_KEY );

		rc = get_keyblock_byfprint( &keyblock,
					    tmp.r.key.fingerprint,
					    tmp.r.key.fingerprint_len );
		if( rc ) {
		    log_error("lid %lu: keyblock not found: %s\n",
						 recnum, g10_errstr(rc) );
		    count++;
		    skip_count++;
		    continue;
		}

		rc = update_trust_record( keyblock, &modified );
		if( rc ) {
		    log_error("lid %lu: update failed: %s\n",
						 recnum, g10_errstr(rc) );
		    err_count++;
		}
		else if( modified ) {
		    if( opt.verbose )
			log_info("lid %lu: updated\n", recnum );
		    upd_count++;
		}
		else if( opt.verbose > 1 )
		    log_info("lid %lu: okay\n", recnum );

		release_kbnode( keyblock ); keyblock = NULL;
		if( !(++count % 100) )
		    log_info(_("%lu keys so far processed\n"), count);
	    }
	}
	log_info(_("%lu keys processed\n"), count);
	if( skip_count )
	    log_info(_("\t%lu keys skipped\n"), skip_count);
	if( err_count )
	    log_info(_("\t%lu keys with errors\n"), err_count);
	if( upd_count )
	    log_info(_("\t%lu keys updated\n"), upd_count);
    }
}


void
update_trustdb( )
{
    KBNODE keyblock = NULL;
    KBPOS kbpos;
    int rc;

    rc = enum_keyblocks( 0, &kbpos, &keyblock );
    if( !rc ) {
	ulong count=0, upd_count=0, err_count=0, new_count=0;

	while( !(rc = enum_keyblocks( 1, &kbpos, &keyblock )) ) {
	    int modified;

	    rc = update_trust_record( keyblock, &modified );
	    if( rc == -1 ) { /* not yet in trustdb: insert */
		PKT_public_key *pk =
			    find_kbnode( keyblock, PKT_PUBLIC_KEY
				       ) ->pkt->pkt.public_key;
		rc = insert_trust_record( pk );
		if( rc && !pk->local_id ) {
		    log_error("lid ?: insert failed: %s\n",
						     g10_errstr(rc) );
		    err_count++;
		}
		else if( rc ) {
		    log_error("lid %lu: insert failed: %s\n",
				       pk->local_id, g10_errstr(rc) );
		    err_count++;
		}
		else {
		    if( opt.verbose )
			log_info("lid %lu: inserted\n", pk->local_id );
		    new_count++;
		}
	    }
	    else if( rc ) {
		log_error("lid %lu: update failed: %s\n",
			 lid_from_keyblock(keyblock), g10_errstr(rc) );
		err_count++;
	    }
	    else if( modified ) {
		if( opt.verbose )
		    log_info("lid %lu: updated\n", lid_from_keyblock(keyblock));
		upd_count++;
	    }
	    else if( opt.verbose > 1 )
		log_info("lid %lu: okay\n", lid_from_keyblock(keyblock) );

	    release_kbnode( keyblock ); keyblock = NULL;
	    if( !(++count % 100) )
		log_info(_("%lu keys so far processed\n"), count);
	}
	log_info(_("%lu keys processed\n"), count);
	if( err_count )
	    log_info(_("\t%lu keys with errors\n"), err_count);
	if( upd_count )
	    log_info(_("\t%lu keys updated\n"), upd_count);
	if( new_count )
	    log_info(_("\t%lu keys inserted\n"), new_count);
    }
    if( rc && rc != -1 )
	log_error("enum_keyblocks failed: %s\n", g10_errstr(rc));

    enum_keyblocks( 2, &kbpos, &keyblock ); /* close */
    release_kbnode( keyblock );
}



/****************
 * Get the trustlevel for this PK.
 * Note: This does not ask any questions
 * Returns: 0 okay of an errorcode
 *
 * It operates this way:
 *  locate the pk in the trustdb
 *	found:
 *	    Do we have a valid cache record for it?
 *		yes: return trustlevel from cache
 *		no:  make a cache record and all the other stuff
 *	not found:
 *	    try to insert the pubkey into the trustdb and check again
 *
 * Problems: How do we get the complete keyblock to check that the
 *	     cache record is actually valid?  Think we need a clever
 *	     cache in getkey.c	to keep track of this stuff. Maybe it
 *	     is not necessary to check this if we use a local pubring. Hmmmm.
 */
int
check_trust( PKT_public_key *pk, unsigned *r_trustlevel )
{
    TRUSTREC rec;
    unsigned trustlevel = TRUST_UNKNOWN;
    int rc=0;
    u32 cur_time;
    u32 keyid[2];


    keyid_from_pk( pk, keyid );

    /* get the pubkey record */
    if( pk->local_id ) {
	read_record( pk->local_id, &rec, RECTYPE_DIR );
    }
    else { /* no local_id: scan the trustdb */
	if( (rc=tdbio_search_dir_bypk( pk, &rec )) && rc != -1 ) {
	    log_error("check_trust: search dir record failed: %s\n",
							    g10_errstr(rc));
	    return rc;
	}
	else if( rc == -1 ) { /* not found - insert */
	    rc = insert_trust_record( pk );
	    if( rc ) {
		log_error(_("key %08lX: insert trust record failed: %s\n"),
					  (ulong)keyid[1], g10_errstr(rc));
		goto leave;
	    }
	    log_info(_("key %08lX.%lu: inserted into trustdb\n"),
					  (ulong)keyid[1], pk->local_id );
	    /* and re-read the dir record */
	    read_record( pk->local_id, &rec, RECTYPE_DIR );
	}
    }
    cur_time = make_timestamp();
    if( pk->timestamp > cur_time ) {
	log_info(_("key %08lX.%lu: created in future "
		   "(time warp or clock problem)\n"),
					  (ulong)keyid[1], pk->local_id );
	return G10ERR_TIME_CONFLICT;
    }

    if( pk->expiredate && pk->expiredate <= cur_time ) {
	log_info(_("key %08lX.%lu: expired at %s\n"),
			(ulong)keyid[1], pk->local_id,
			     asctimestamp( pk->expiredate) );
	 trustlevel = TRUST_EXPIRED;
    }
    else {
	rc = do_check( &rec, &trustlevel );
	if( rc ) {
	    log_error(_("key %08lX.%lu: trust check failed: %s\n"),
			    (ulong)keyid[1], pk->local_id, g10_errstr(rc));
	    return rc;
	}
    }


  leave:
    if( DBG_TRUST )
	log_debug("check_trust() returns trustlevel %04x.\n", trustlevel);
    *r_trustlevel = trustlevel;
    return 0;
}


int
query_trust_info( PKT_public_key *pk )
{
    unsigned trustlevel;
    int c;

    if( check_trust( pk, &trustlevel ) )
	return '?';
    if( trustlevel & TRUST_FLAG_REVOKED )
	return 'r';
    switch( (trustlevel & TRUST_MASK) ) {
      case TRUST_UNKNOWN:   c = 'o'; break;
      case TRUST_EXPIRED:   c = 'e'; break;
      case TRUST_UNDEFINED: c = 'q'; break;
      case TRUST_NEVER:     c = 'n'; break;
      case TRUST_MARGINAL:  c = 'm'; break;
      case TRUST_FULLY:     c = 'f'; break;
      case TRUST_ULTIMATE:  c = 'u'; break;
      default: BUG();
    }
    return c;
}



/****************
 * Enumerate all keys, which are needed to build all trust paths for
 * the given key.  This function does not return the key itself or
 * the ultimate key.
 *
 *  1) create a void pointer and initialize it to NULL
 *  2) pass this void pointer by reference to this function.
 *     Set lid to the key you want to enumerate and pass it by reference.
 *  3) call this function as long as it does not return -1
 *     to indicate EOF. LID does contain the next key used to build the web
 *  4) Always call this function a last time with LID set to NULL,
 *     so that it can free its context.
 */
int
enum_trust_web( void **context, ulong *lid )
{
    /* REPLACE THIS with a BETTER ONE  */

    return -1; /* eof */
}


/****************
 * Return the assigned ownertrust value for the given LID
 */
unsigned
get_ownertrust( ulong lid )
{
    TRUSTREC rec;

    read_record( lid, &rec, RECTYPE_DIR );
    return rec.r.dir.ownertrust;
}

int
get_ownertrust_info( ulong lid )
{
    unsigned otrust;
    int c;

    otrust = get_ownertrust( lid );
    switch( (otrust & TRUST_MASK) ) {
      case TRUST_NEVER:     c = 'n'; break;
      case TRUST_MARGINAL:  c = 'm'; break;
      case TRUST_FULLY:     c = 'f'; break;
      case TRUST_ULTIMATE:  c = 'u'; break;
      default:		    c = '-'; break;
    }
    return c;
}


byte *
get_pref_data( ulong lid, const byte *namehash, size_t *ret_n )
{
    TRUSTREC rec;
    ulong recno;

    read_record( lid, &rec, RECTYPE_DIR );
    for( recno=rec.r.dir.uidlist; recno; recno = rec.r.uid.next ) {
	read_record( recno, &rec, RECTYPE_UID );
	if( rec.r.uid.prefrec
	    && ( !namehash || !memcmp(namehash, rec.r.uid.namehash, 20) ))  {
	    byte *buf;
	    /* found the correct one or the first one */
	    read_record( rec.r.uid.prefrec, &rec, RECTYPE_PREF );
	    if( rec.r.pref.next )
		log_info("warning: can't yet handle long pref records\n");
	    buf = m_alloc( ITEMS_PER_PREF_RECORD );
	    memcpy( buf, rec.r.pref.data, ITEMS_PER_PREF_RECORD );
	    *ret_n = ITEMS_PER_PREF_RECORD;
	    return buf;
	}
    }
    return NULL;
}



/****************
 * Check whether the algorithm is in one of the pref records
 */
int
is_algo_in_prefs( ulong lid, int preftype, int algo )
{
    TRUSTREC rec;
    ulong recno;
    int i;
    byte *pref;

    read_record( lid, &rec, RECTYPE_DIR );
    for( recno=rec.r.dir.uidlist; recno; recno = rec.r.uid.next ) {
	read_record( recno, &rec, RECTYPE_UID );
	if( rec.r.uid.prefrec ) {
	    read_record( rec.r.uid.prefrec, &rec, RECTYPE_PREF );
	    if( rec.r.pref.next )
		log_info("warning: can't yet handle long pref records\n");
	    pref = rec.r.pref.data;
	    for(i=0; i+1 < ITEMS_PER_PREF_RECORD; i+=2 ) {
		if( pref[i] == preftype && pref[i+1] == algo )
		    return 1;
	    }
	}
    }
    return 0;
}


static int
get_dir_record( PKT_public_key *pk, TRUSTREC *rec )
{
    int rc=0;

    if( pk->local_id ) {
	read_record( pk->local_id, rec, RECTYPE_DIR );
    }
    else { /* no local_id: scan the trustdb */
	if( (rc=tdbio_search_dir_bypk( pk, rec )) && rc != -1 )
	    log_error("get_dir_record: search_record failed: %s\n",
							    g10_errstr(rc));
    }
    return rc;
}



/****************
 * This function simply looks for the key in the trustdb
 * and makes sure that pk->local_id is set to the coreect value.
 * Return: 0 = found
 *	   -1 = not found
 *	  other = error
 */
int
query_trust_record( PKT_public_key *pk )
{
    TRUSTREC rec;
    return get_dir_record( pk, &rec );
}

/* FIXME: Brauchen wir das?? */
int
clear_trust_checked_flag( PKT_public_key *pk )
{
    TRUSTREC rec;
    int rc;

    rc = get_dir_record( pk, &rec );
    if( rc )
	return rc;

    if( !(rec.r.dir.dirflags & DIRF_CHECKED) )
	return 0;

    /* reset the flag */
    rec.r.dir.dirflags &= ~DIRF_CHECKED;
    write_record( &rec );
    do_sync();
    return 0;
}




static void
check_hint_sig( ulong lid, KBNODE keyblock, u32 *keyid, byte *uidrec_hash,
		TRUSTREC *sigrec, int sigidx, ulong hint_owner )
{
    KBNODE node;
    int rc, state;
    byte uhash[20];
    int is_selfsig;
    PKT_signature *sigpkt = NULL;
    TRUSTREC tmp;
    u32 sigkid[2];

    if( sigrec->r.sig.sig[sigidx].flag & SIGF_CHECKED )
	log_info(_("note: sig rec %lu[%d] in hintlist "
		   "of %lu but marked as checked\n"),
		    sigrec->recnum, sigidx, hint_owner );
    if( !(sigrec->r.sig.sig[sigidx].flag & SIGF_NOPUBKEY) )
	log_info(_("note: sig rec %lu[%d] in hintlist "
		   "of %lu but not marked\n"),
		    sigrec->recnum, sigidx, hint_owner );

    read_record( sigrec->r.sig.sig[sigidx].lid, &tmp, 0 );
    if( tmp.rectype != RECTYPE_DIR ) {
	/* we need the dir record */
	log_error(_("sig rec %lu[%d] in hintlist "
		    "of %u does not point to a dir record\n"),
		    sigrec->recnum, sigidx, hint_owner );
	return;
    }
    if( !tmp.r.dir.keylist ) {
	log_error(_("lid %lu: no primary key\n"), tmp.r.dir.lid );
	return;
    }
    read_record(tmp.r.dir.keylist, &tmp, RECTYPE_KEY );
    keyid_from_fingerprint( tmp.r.key.fingerprint,
			    tmp.r.key.fingerprint_len, sigkid );


    /* find the correct signature packet */
    state = 0;
    for( node=keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_USER_ID ) {
	    PKT_user_id *uidpkt = node->pkt->pkt.user_id;

	    if( state )
		break;
	    rmd160_hash_buffer( uhash, uidpkt->name, uidpkt->len );
	    if( !memcmp( uhash, uidrec_hash, 20 ) )
		state = 1;
	}
	else if( state && node->pkt->pkttype == PKT_SIGNATURE ) {
	    sigpkt = node->pkt->pkt.signature;
	    if( sigpkt->keyid[0] == sigkid[0]
		&& sigpkt->keyid[1] == sigkid[1]
		&& (sigpkt->sig_class&~3) == 0x10 ) {
		state = 2;
		break; /* found */
	    }
	}
    }

    if( !node ) {
	log_info(_("lid %lu: user id not found in keyblock\n"), lid );
	return ;
    }
    if( state != 2 ) {
	log_info(_("lid %lu: user id without signature\n"), lid );
	return ;
    }

    /* and check the sig */
    rc = check_key_signature( keyblock, node, &is_selfsig );
    if( is_selfsig ) {
	log_error(_("lid %lu: self-signature in hintlist\n"), lid );
	return;
    }
    if( !rc ) { /* valid signature */
	if( opt.verbose )
	    log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX: "
		       "Good signature (3)\n"),
		    (ulong)keyid[1], lid, uhash[18], uhash[19],
		    (ulong)sigpkt->keyid[1] );
	sigrec->r.sig.sig[sigidx].flag = SIGF_CHECKED | SIGF_VALID;
    }
    else if( rc == G10ERR_NO_PUBKEY ) {
	log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX: "
		   "very strange: no public key\n"),
	      (ulong)keyid[1], lid, uhash[18], uhash[19],
	       (ulong)sigpkt->keyid[1] );
	sigrec->r.sig.sig[sigidx].flag = SIGF_NOPUBKEY;
    }
    else {
	log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX: %s\n"),
		    (ulong)keyid[1], lid, uhash[18], uhash[19],
		    (ulong)sigpkt->keyid[1], g10_errstr(rc) );
	sigrec->r.sig.sig[sigidx].flag = SIGF_CHECKED;
    }
    sigrec->dirty = 1;
}


/****************
 * Process a hintlist.
 * Fixme: this list is not anymore anchored to another
 *	  record, so it should be put elsewehere in case of an error
 */
static void
process_hintlist( ulong hintlist, ulong hint_owner )
{
    ulong hlst_rn;
    int rc;

    for( hlst_rn = hintlist; hlst_rn; ) {
	TRUSTREC hlstrec;
	int hlst_idx;

	read_record( hlst_rn, &hlstrec, RECTYPE_HLST );

	for( hlst_idx=0; hlst_idx < ITEMS_PER_HLST_RECORD; hlst_idx++ ) {
	    TRUSTREC dirrec;
	    TRUSTREC uidrec;
	    TRUSTREC tmprec;
	    KBNODE keyblock = NULL;
	    u32 keyid[2];
	    ulong lid;
	    ulong r1, r2;

	    lid = hlstrec.r.hlst.rnum[hlst_idx];
	    if( !lid )
		continue;

	    read_record( lid, &dirrec, 0 );
	    /* make sure it points to a dir record:
	     * this should be true because it only makes sense to
	     * call this function if the dir record is available */
	    if( dirrec.rectype != RECTYPE_DIR )  {
		log_error(_("hintlist %lu[%d] of %lu "
			    "does not point to a dir record\n"),
			    hlst_rn, hlst_idx, hint_owner );
		continue;
	    }
	    if( !dirrec.r.dir.keylist ) {
		log_error(_("lid %lu does not have a key\n"), lid );
		continue;
	    }

	    /* get the keyblock */
	    read_record( dirrec.r.dir.keylist, &tmprec, RECTYPE_KEY );
	    rc = get_keyblock_byfprint( &keyblock,
					tmprec.r.key.fingerprint,
					tmprec.r.key.fingerprint_len );
	    if( rc ) {
		log_error(_("lid %lu: can't get keyblock: %s\n"),
						    lid, g10_errstr(rc) );
		continue;
	    }
	    keyid_from_fingerprint( tmprec.r.key.fingerprint,
				    tmprec.r.key.fingerprint_len, keyid );

	    /* Walk over all user ids and their signatures and check all
	     * the signature which are created by hint_owner */
	    for( r1 = dirrec.r.dir.uidlist; r1; r1 = uidrec.r.uid.next ) {
		TRUSTREC sigrec;

		read_record( r1, &uidrec, RECTYPE_UID );
		for( r2 = uidrec.r.uid.siglist; r2; r2 = sigrec.r.sig.next ) {
		    int i;

		    read_record( r2, &sigrec, RECTYPE_SIG );
		    sigrec.dirty = 0;
		    for(i=0; i < SIGS_PER_RECORD; i++ ) {
			if( !sigrec.r.sig.sig[i].lid )
			    continue; /* skip deleted sigs */
			if( sigrec.r.sig.sig[i].lid != hint_owner )
			    continue; /* not for us */
			/* some diagnostic messages */
			/* and do the signature check */
			check_hint_sig( lid, keyblock, keyid,
					uidrec.r.uid.namehash,
					&sigrec, i, hint_owner );
		    }
		    if( sigrec.dirty )
			write_record( &sigrec );
		}
	    }
	    release_kbnode( keyblock );
	} /* loop over hlst entries */

	/* delete this hlst record */
	hlst_rn = hlstrec.r.hlst.next;
	delete_record( hlstrec.recnum );
    } /* loop over hintlist */
}


/****************
 * Create or update shadow dir record and return the LID of the record
 */
static ulong
create_shadow_dir( PKT_signature *sig, ulong lid  )
{
    TRUSTREC sdir, hlst, tmphlst;
    ulong recno, newlid;
    int tmpidx;
    int rc;

    /* first see whether we already have such a record */
    rc = tdbio_search_sdir( sig->keyid, sig->pubkey_algo, &sdir );
    if( rc && rc != -1 ) {
	log_error("tdbio_search_dir failed: %s\n", g10_errstr(rc));
	die_invalid_db();
    }
    if( rc == -1 ) { /* not found: create */
	memset( &sdir, 0, sizeof sdir );
	sdir.recnum = tdbio_new_recnum();
	sdir.rectype= RECTYPE_SDIR;
	sdir.r.sdir.lid = sdir.recnum;
	sdir.r.sdir.keyid[0] = sig->keyid[0];
	sdir.r.sdir.keyid[1] = sig->keyid[1];
	sdir.r.sdir.pubkey_algo = sig->pubkey_algo;
	sdir.r.sdir.hintlist = 0;
	write_record( &sdir );
    }
    newlid = sdir.recnum;
    /* Put the record number into the hintlist.
     * (It is easier to use the lid and not the record number of the
     *	key to save some space (assuming that a signator has
     *	signed more than one user id - and it is easier to implement.)
     */
    tmphlst.recnum = 0;
    for( recno=sdir.r.sdir.hintlist; recno; recno = hlst.r.hlst.next) {
	int i;
	read_record( recno, &hlst, RECTYPE_HLST );
	for( i=0; i < ITEMS_PER_HLST_RECORD; i++ ) {
	    if( !hlst.r.hlst.rnum[i] ) {
		if( !tmphlst.recnum ) {
		    tmphlst = hlst;
		    tmpidx = i;
		}
	    }
	    else if( hlst.r.hlst.rnum[i] == lid )
		return newlid; /* the signature is already in the hintlist */
	}
    }
    /* not yet in the hint list, write it */
    if( tmphlst.recnum ) { /* we have an empty slot */
	tmphlst.r.hlst.rnum[tmpidx] = lid;
	write_record( &tmphlst );
    }
    else { /* must append a new hlst record */
	memset( &hlst, 0, sizeof hlst );
	hlst.recnum = tdbio_new_recnum();
	hlst.rectype = RECTYPE_HLST;
	hlst.r.hlst.next = sdir.r.sdir.hintlist;
	hlst.r.hlst.rnum[0] = lid;
	write_record( &hlst );
	sdir.r.sdir.hintlist = hlst.recnum;
	write_record( &sdir );
    }

    return newlid;
}


static void
upd_key_record( PKT_public_key *pk, TRUSTREC *drec, RECNO_LIST *recno_list )
{
    TRUSTREC krec;
    byte fpr[MAX_FINGERPRINT_LEN];
    size_t fprlen;
    ulong recno, newrecno;

    fingerprint_from_pk( pk, fpr, &fprlen );
    /* do we already have this key? */
    for( recno=drec->r.dir.keylist; recno; recno = krec.r.key.next ) {
	read_record( recno, &krec, RECTYPE_KEY );
	if( krec.r.key.fingerprint_len == fprlen
	    && !memcmp( krec.r.key.fingerprint, fpr, fprlen ) )
	    break;
    }
    if( recno ) { /* yes */
	ins_recno_list( recno_list, recno, RECTYPE_KEY );
    }
    else { /* no: insert this new key */
	memset( &krec, 0, sizeof(krec) );
	krec.rectype = RECTYPE_KEY;
	krec.r.key.lid = drec->recnum;
	krec.r.key.pubkey_algo = pk->pubkey_algo;
	krec.r.key.fingerprint_len = fprlen;
	memcpy(krec.r.key.fingerprint, fpr, fprlen );
	krec.recnum = newrecno = tdbio_new_recnum();
	write_record( &krec );
	ins_recno_list( recno_list, newrecno, RECTYPE_KEY );
	/* and put this new record at the end of the keylist */
	if( !(recno=drec->r.dir.keylist) ) {
	    /* this is the first key */
	    drec->r.dir.keylist = newrecno;
	    drec->dirty = 1;
	}
	else { /* we already have a key, append it to the list */
	    for( ; recno; recno = krec.r.key.next )
		read_record( recno, &krec, RECTYPE_KEY );
	    krec.r.key.next = newrecno;
	    write_record( &krec );
	}
    }
}


static void
upd_uid_record( PKT_user_id *uid, TRUSTREC *drec, RECNO_LIST *recno_list,
			      u32 *keyid, ulong *uidrecno, byte *uidhash )
{
    TRUSTREC urec;
    ulong recno, newrecno;

    rmd160_hash_buffer( uidhash, uid->name, uid->len );
    for( recno=drec->r.dir.uidlist; recno; recno = urec.r.uid.next ) {
	read_record( recno, &urec, RECTYPE_UID );
	if( !memcmp( uidhash, urec.r.uid.namehash, 20 ) )
	    break;
    }
    if( recno ) {
	ins_recno_list( recno_list, recno, RECTYPE_UID );
	*uidrecno = recno;
    }
    else { /* new user id */
	memset( &urec, 0 , sizeof(urec) );
	urec.rectype = RECTYPE_UID;
	urec.r.uid.lid = drec->recnum;
	memcpy(urec.r.uid.namehash, uidhash, 20 );
	urec.recnum = newrecno = tdbio_new_recnum();
	write_record( &urec );
	ins_recno_list( recno_list, newrecno, RECTYPE_UID );
	/* and put this new record at the end of the uidlist */
	if( !(recno=drec->r.dir.uidlist) ) { /* this is the first uid */
	    drec->r.dir.uidlist = newrecno;
	    drec->dirty = 1;
	}
	else { /* we already have an uid, append it to the list */
	    for( ; recno; recno = urec.r.key.next )
		read_record( recno, &urec, RECTYPE_UID );
	    urec.r.uid.next = newrecno;
	    write_record( &urec );
	}
	*uidrecno = newrecno;
    }
}


static void
upd_pref_record( PKT_signature *sig, ulong lid, const u32 *keyid,
				TRUSTREC *urec, const byte *uidhash )
{
    static struct {
	sigsubpkttype_t subpkttype;
	int preftype;
    } prefs[] = {
	{ SIGSUBPKT_PREF_SYM,	PREFTYPE_SYM	},
	{ SIGSUBPKT_PREF_HASH,	PREFTYPE_HASH	},
	{ SIGSUBPKT_PREF_COMPR, PREFTYPE_COMPR	},
	{ 0, 0 }
    };
    TRUSTREC prec;
    const byte *s;
    size_t n;
    int k, i;
    ulong recno_tbl[10];
    int recno_idx = 0;
    ulong recno;

    /* First delete all pref records
     * This is much simpler than checking whether we have to
     * do update the record at all - the record cache may care about it
     * FIXME: We never get correct statistics if we di it like this */
    for( recno=urec->r.uid.prefrec; recno; recno = prec.r.pref.next ) {
	read_record( recno, &prec, RECTYPE_PREF );
	delete_record( recno );
    }

    /* and write the new ones */
    i = 0;
    for(k=0; prefs[k].subpkttype; k++ ) {
	s = parse_sig_subpkt2( sig, prefs[k].subpkttype, &n );
	if( s ) {
	    while( n ) {
		if( !i || i >= ITEMS_PER_PREF_RECORD ) {
		    if( recno_idx >= DIM(recno_tbl)-1 ) {
			log_info("too many preferences\n");
			break;
		    }
		    if( i ) {
			recno_tbl[recno_idx]=tdbio_new_recnum();
			prec.recnum = recno_tbl[recno_idx++];
			write_record( &prec );
		    }
		    memset( &prec, 0, sizeof prec );
		    prec.rectype = RECTYPE_PREF;
		    prec.r.pref.lid = lid;
		    i = 0;
		}
		prec.r.pref.data[i++] = prefs[k].preftype;
		prec.r.pref.data[i++] = *s++;
		n--;
	    }
	}
    }
    if( i ) { /* write the last one */
	recno_tbl[recno_idx]=tdbio_new_recnum();
	prec.recnum = recno_tbl[recno_idx++];
	write_record( &prec );
    }
    /* now link them together */
    for(i=0; i < recno_idx-1; i++ ) {
	read_record( recno_tbl[i], &prec, RECTYPE_PREF );
	prec.r.pref.next = recno_tbl[i+1];
	write_record( &prec );
    }
    /* don't need to write the last one, but update the uid */
    urec->r.uid.prefrec = recno_idx? recno_tbl[0] : 0;
    urec->dirty = 1;
}


/****************
 * update self key signatures (class 0x10..0x13)
 */
static void
upd_self_key_sigs( PKT_signature *sig, TRUSTREC *urec,
		   ulong lid, const u32 *keyid,  const byte *uidhash,
		   KBNODE keyblock, KBNODE signode)
{
    int rc;

    /* must verify this selfsignature here, so that we can
     * build the preference record and validate the uid record
     */
    if( !(urec->r.uid.uidflags & UIDF_CHECKED) ) {
	rc = check_key_signature( keyblock, signode, NULL );
	if( !rc ) {
	    if( opt.verbose )
		log_info(_(
		     "key %08lX.%lu, uid %02X%02X: Good self-signature\n"),
		   (ulong)keyid[1], lid, uidhash[18], uidhash[19] );
	    upd_pref_record( sig, lid, keyid, urec, uidhash );
	    urec->r.uid.uidflags = UIDF_CHECKED | UIDF_VALID;
	}
	else {
	    log_info(_("key %08lX, uid %02X%02X: Invalid self-signature: %s\n"),
		    (ulong)keyid[1], uidhash[18], uidhash[19], g10_errstr(rc) );
	    urec->r.uid.uidflags = UIDF_CHECKED;
	}
	urec->dirty = 1;
    }
}


/****************
 * update non-self key signatures (class 0x10..0x13)
 */
static void
upd_nonself_key_sigs( PKT_signature *sig, TRUSTREC *urec,
		      ulong lid, const u32 *keyid,  const byte *uidhash,
		      KBNODE keyblock, KBNODE signode)
{
    /* We simply insert the signature into the sig records but
     * avoid duplicate ones.  We do not check them here because
     * there is a big chance, that we import required public keys
     * later.  The problem with this is that we must somewhere store
     * the information about this signature (we need a record id).
     * We do this by using the record type shadow dir, which will
     * be converted to a dir record as soon as a new public key is
     * inserted into the trustdb.
     */
    TRUSTREC rec;
    ulong recno;
    TRUSTREC delrec;
    int delrecidx;
    int newflag = 0;
    ulong newlid = 0;
    PKT_public_key *pk = m_alloc_clear( sizeof *pk );
    ulong pk_lid = 0;
    int found_sig = 0;
    int found_delrec = 0;
    int rc;

    delrec.recnum = 0;

    /* get the LID of the pubkey of the signature under verification */
    rc = get_pubkey( pk, sig->keyid );
    if( !rc ) {
	if( pk->local_id )
	    pk_lid = pk->local_id;
	else {
	    rc = tdbio_search_dir_bypk( pk, &rec );
	    if( !rc )
		pk_lid = rec.recnum;
	    else if( rc == -1 ) { /* see whether there is a sdir instead */
		u32 akid[2];

		keyid_from_pk( pk, akid );
		rc = tdbio_search_sdir( akid, pk->pubkey_algo, &rec );
		if( !rc )
		    pk_lid = rec.recnum;
	    }
	}
    }
    free_public_key( pk ); pk = NULL;

    /* Loop over all signatures just in case one is not correctly
     * marked.	If we see the correct signature, set a flag.
     * delete duplicate signatures (should not happen but...) */
    for( recno = urec->r.uid.siglist; recno; recno = rec.r.sig.next ) {
	int i;

	read_record( recno, &rec, RECTYPE_SIG );
	for(i=0; i < SIGS_PER_RECORD; i++ ) {
	    TRUSTREC tmp;
	    if( !rec.r.sig.sig[i].lid ) {
		if( !found_delrec && !delrec.recnum ) {
		    delrec = rec;
		    delrecidx = i;
		    found_delrec=1;
		}
		continue; /* skip deleted sigs */
	    }
	    if( rec.r.sig.sig[i].lid == pk_lid ) {
		if( found_sig ) {
		    log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX: "
			       "duplicated signature - deleted\n"),
			  (ulong)keyid[1], lid, uidhash[18],
			   uidhash[19], (ulong)sig->keyid[1] );
		    rec.r.sig.sig[i].lid = 0;
		    rec.dirty = 1;
		    continue;
		}
		found_sig = 1;
	    }
	    if( rec.r.sig.sig[i].flag & SIGF_CHECKED )
		continue; /* we already checked this signature */
	    if( rec.r.sig.sig[i].flag & SIGF_NOPUBKEY )
		continue; /* we do not have the public key */

	    read_record( rec.r.sig.sig[i].lid, &tmp, 0 );
	    if( tmp.rectype == RECTYPE_DIR ) {
		/* In this case we should now be able to check the signature */
		rc = check_key_signature( keyblock, signode, NULL );
		if( !rc ) { /* valid signature */
		    if( opt.verbose )
			log_info(_(
			       "key %08lX.%lu, uid %02X%02X, sig %08lX: "
			       "Good signature (1)\n"),
				(ulong)keyid[1], lid, uidhash[18],
				uidhash[19], (ulong)sig->keyid[1] );
		    rec.r.sig.sig[i].flag = SIGF_CHECKED | SIGF_VALID;
		}
		else if( rc == G10ERR_NO_PUBKEY ) {
		    log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX: "
			       "weird: no public key\n"),
			  (ulong)keyid[1], lid, uidhash[18],
			   uidhash[19], (ulong)sig->keyid[1] );
		    rec.r.sig.sig[i].flag = SIGF_NOPUBKEY;
		}
		else {
		    log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX:"
			       " %s\n"),
				(ulong)keyid[1], lid, uidhash[18],
				uidhash[19], (ulong)sig->keyid[1],
						    g10_errstr(rc));
		    rec.r.sig.sig[i].flag = SIGF_CHECKED;
		}
		rec.dirty = 1;
	    }
	    else if( tmp.rectype == RECTYPE_SDIR ) {
		/* must check that it is the right one */
		if( tmp.r.sdir.keyid[0] == sig->keyid[0]
		    && tmp.r.sdir.keyid[1] == sig->keyid[1]
		    && (!tmp.r.sdir.pubkey_algo
			 || tmp.r.sdir.pubkey_algo == sig->pubkey_algo )) {
		    log_info(_("key %08lX.%lu, uid %02X%02X: "
			       "has shadow dir %lu but not yet marked.\n"),
				(ulong)keyid[1], lid,
				uidhash[18], uidhash[19], tmp.recnum );
		    rec.r.sig.sig[i].flag = SIGF_NOPUBKEY;
		    rec.dirty = 1;
		    /* fixme: should we verify that the record is
		     * in the hintlist? - This case here should anyway
		     * never occur */
		}
	    }
	    else {
		log_error("sig record %lu[%d] points to wrong record.\n",
			    rec.r.sig.sig[i].lid, i );
		die_invalid_db();
	    }
	}
	if( found_delrec && delrec.recnum ) {
	    delrec = rec;
	    found_delrec = 0; /* we only want the first one */
	}
	if( rec.dirty ) {
	    write_record( &rec );
	    rec.dirty = 0;
	}
    }

    if( found_sig )
	return;

    /* at this point, we have verified, that the signature is not in
     * our list of signatures.	Add a new record with that signature
     * and if the public key is there, check the signature. */

    if( !pk_lid ) /* we have already seen that there is no pubkey */
	rc = G10ERR_NO_PUBKEY;
    else
	rc = check_key_signature( keyblock, signode, NULL );

    if( !rc ) { /* valid signature */
	if( opt.verbose )
	    log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX: "
		       "Good signature (2)\n"),
			  (ulong)keyid[1], lid, uidhash[18],
			   uidhash[19], (ulong)sig->keyid[1] );
	newlid = pk_lid;  /* this is the pk of the signature */
	newflag = SIGF_CHECKED | SIGF_VALID;
    }
    else if( rc == G10ERR_NO_PUBKEY ) {
	if( opt.verbose > 1 )
	    log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX: "
		       "no public key\n"),
			  (ulong)keyid[1], lid, uidhash[18],
			   uidhash[19], (ulong)sig->keyid[1] );
	newlid = create_shadow_dir( sig, lid );
	newflag = SIGF_NOPUBKEY;
    }
    else {
	log_info(_("key %08lX.%lu, uid %02X%02X, sig %08lX: %s\n"),
		    (ulong)keyid[1], lid, uidhash[18], uidhash[19],
			      (ulong)sig->keyid[1], g10_errstr(rc));
	newlid = create_shadow_dir( sig, lid );
	newflag = SIGF_CHECKED;
    }

    if( delrec.recnum ) { /* we can reuse a deleted slot */
	delrec.r.sig.sig[delrecidx].lid = newlid;
	delrec.r.sig.sig[delrecidx].flag= newflag;
	write_record( &delrec );
    }
    else { /* must insert a new sig record */
	TRUSTREC tmp;

	memset( &tmp, 0, sizeof tmp );
	tmp.recnum = tdbio_new_recnum();
	tmp.rectype = RECTYPE_SIG;
	tmp.r.sig.lid = lid;
	tmp.r.sig.next = urec->r.uid.siglist;
	tmp.r.sig.sig[0].lid = newlid;
	tmp.r.sig.sig[0].flag= newflag;
	write_record( &tmp );
	urec->r.uid.siglist = tmp.recnum;
	urec->dirty = 1;
    }
}



/****************
 * Note: A signature made with a secondary key is not considered a
 *	 self-signature.
 */
static void
upd_sig_record( PKT_signature *sig, TRUSTREC *drec,
		u32 *keyid, ulong *uidrecno, byte *uidhash,
		KBNODE keyblock, KBNODE signode)
{
    TRUSTREC urec;
    ulong lid = drec->recnum;

    if( !*uidrecno ) {
	switch( sig->sig_class ) {
	  case 0x20:
	  case 0x28: /* We do not need uids for [sub]key revications */
	  case 0x18: /* or subkey binding */
	    memset( &urec, 0, sizeof urec ); /* to catch errors */
	    break;

	  default:
	    log_error("key %08lX: signature (class %02x) without user id\n",
					 (ulong)keyid[1], sig->sig_class );
	    return;
	}
    }
    else
	read_record( *uidrecno, &urec, RECTYPE_UID );

    if( keyid[0] == sig->keyid[0] && keyid[1] == sig->keyid[1] ) {
	if( (sig->sig_class&~3) == 0x10 ) {
	    upd_self_key_sigs( sig, &urec, lid, keyid, uidhash,
						keyblock, signode );
	}
	else if( sig->sig_class == 0x18 ) { /* key binding */
	    /* get the corresponding key */

	    /* FIXME */
	}
	else if( sig->sig_class == 0x20 ) { /* key revocation */
	    /* FIXME */
	}
	else if( sig->sig_class == 0x28 ) { /* subkey revocation */
	    /* FIXME */
	}
	else if( sig->sig_class == 0x30 ) { /* cert revocation */
	    /* FIXME */
	}
    }
    else if( (sig->sig_class&~3) == 0x10 ) {
	upd_nonself_key_sigs( sig, &urec, lid, keyid, uidhash,
						keyblock, signode );
    }
    else if( sig->sig_class == 0x18 ) { /* key binding */
	log_info(_("key %08lX: bogus key binding by %08lX\n"),
				 (ulong)keyid[1], (ulong)sig->keyid[1] );
    }
    else if( sig->sig_class == 0x20 ) { /* key revocation */
	log_info(_("key %08lX: bogus key revocation by %08lX\n"),
				 (ulong)keyid[1], (ulong)sig->keyid[1] );
    }
    else if( sig->sig_class == 0x28 ) { /* subkey revocation */
	log_info(_("key %08lX: bogus subkey revocation by %08lX\n"),
				 (ulong)keyid[1], (ulong)sig->keyid[1] );
    }
    else if( sig->sig_class == 0x30 ) { /* cert revocation */
	/* fixme: a signator wants to revoke his certification signature */
    }

    if( urec.dirty ) {
	write_record( &urec );
	urec.dirty = 0;
    }
}


/****************
 * Update all the info from the public keyblock.
 * The key must already exist in the keydb.
 * This function is responsible for checking the signatures in cases
 * where the public key is already available.  If we no not have the public
 * key, the check is done by some special code in insert_trust_record().
 */
int
update_trust_record( KBNODE keyblock, int *modified )
{
    PKT_public_key *primary_pk;
    KBNODE node;
    TRUSTREC drec;
    TRUSTREC krec;
    TRUSTREC urec;
    TRUSTREC prec;
    TRUSTREC helprec;
    int rc = 0;
    u32 keyid[2]; /* keyid of primary key */
    ulong recno, lastrecno;
    ulong uidrecno = 0;
    byte uidhash[20];
    RECNO_LIST recno_list = NULL; /* list of verified records */
    /* fixme: replace recno_list by a lookup on node->recno */

    if( modified )
	*modified = 0;

    node = find_kbnode( keyblock, PKT_PUBLIC_KEY );
    primary_pk = node->pkt->pkt.public_key;
    rc = get_dir_record( primary_pk, &drec );
    if( rc )
	return rc;
    if( !primary_pk->local_id )
	primary_pk->local_id = drec.recnum;

    keyid_from_pk( primary_pk, keyid );

    rc = tdbio_begin_transaction();
    if( rc )
	return rc;

    /* now update keys and user ids */
    for( node=keyblock; node; node = node->next ) {
	switch( node->pkt->pkttype ) {
	  case PKT_PUBLIC_KEY:
	  case PKT_PUBLIC_SUBKEY:
	    uidrecno = 0;
	    upd_key_record( node->pkt->pkt.public_key, &drec, &recno_list );
	    break;

	  case PKT_USER_ID:
	    if( drec.dirty ) { /* upd_pref_record may read the drec */
		write_record( &drec );
		drec.dirty = 0;
	    }
	    upd_uid_record( node->pkt->pkt.user_id, &drec, &recno_list,
					     keyid, &uidrecno, uidhash );
	    break;

	  case PKT_SIGNATURE:
	    if( drec.dirty ) { /* upd_sig_recory may read the drec */
		write_record( &drec );
		drec.dirty = 0;
	    }
	    upd_sig_record( node->pkt->pkt.signature, &drec, keyid,
				   &uidrecno, uidhash, keyblock, node );
	    break;

	  default:
	    break;
	}
    } /* end loop over all nodes */


    /* delete keyrecords from the trustdb which are not anymore used */
    lastrecno = 0;
    for( recno=drec.r.dir.keylist; recno; recno = krec.r.key.next ) {
	read_record( recno, &krec, RECTYPE_KEY );
	if( !qry_recno_list( recno_list, recno, RECTYPE_KEY ) ) {
	    /* delete this one */
	    if( !lastrecno ) {
		drec.r.dir.keylist = krec.r.key.next;
		drec.dirty = 1;
	    }
	    else {
		read_record( lastrecno, &helprec, RECTYPE_KEY );
		helprec.r.key.next = krec.r.key.next;
		write_record( &helprec );
	    }
	    delete_record( recno );
	}
	else
	    lastrecno = recno;
    }
    /* delete uid records and sig and their pref records from the
     * trustdb which are not anymore used */
    lastrecno = 0;
    for( recno=drec.r.dir.uidlist; recno; recno = urec.r.uid.next ) {
	read_record( recno, &urec, RECTYPE_UID );
	if( !qry_recno_list( recno_list, recno, RECTYPE_UID ) ) {
	    ulong r2;
	    /* delete this one */
	    if( !lastrecno ) {
		drec.r.dir.uidlist = urec.r.uid.next;
		drec.dirty = 1;
	    }
	    else {
		read_record( lastrecno, &helprec, RECTYPE_UID );
		helprec.r.uid.next = urec.r.uid.next;
		write_record( &helprec );
	    }
	    for(r2=urec.r.uid.prefrec ; r2; r2 = prec.r.pref.next ) {
		read_record( r2, &prec, RECTYPE_PREF );
		delete_record( r2 );
	    }
	    for(r2=urec.r.uid.siglist ; r2; r2 = helprec.r.sig.next ) {
		read_record( r2, &helprec, RECTYPE_SIG );
		delete_record( r2 );
	    }
	    delete_record( recno );
	}
	else
	    lastrecno = recno;
    }



    if( rc )
	rc = tdbio_cancel_transaction();
    else {
	if( drec.dirty ) {
	    drec.r.dir.dirflags &= ~DIRF_CHECKED; /* reset flag */
	    write_record( &drec );
	}
	if( modified && tdbio_is_dirty() )
	    *modified = 0;
	rc = tdbio_end_transaction();
    }
    rel_recno_list( &recno_list );
    return rc;
}


/****************
 * Insert a trust record into the TrustDB
 * This function assumes that the record does not yet exist.
 */
int
insert_trust_record( PKT_public_key *pk )
{
    TRUSTREC dirrec;
    TRUSTREC shadow;
    KBNODE keyblock = NULL;
    KBNODE node;
    byte fingerprint[MAX_FINGERPRINT_LEN];
    size_t fingerlen;
    int rc = 0;
    ulong hintlist = 0;

    if( pk->local_id )
	log_bug("pk->local_id=%lu\n", pk->local_id );

    fingerprint_from_pk( pk, fingerprint, &fingerlen );

    /* fixme: assert that we do not have this record.
     * we can do this by searching for the primary keyid
     *
     * fixme: If there is no such key we should look whether one
     * of the subkeys has been used to sign another key and in this case
     * we got the key anyway.  Because a secondary key can't be used
     * without a primary key (it is needed to bind the secondary one
     * to the primary one which has the user ids etc.)
     */

    /* get the keyblock which has the key */
    rc = get_keyblock_byfprint( &keyblock, fingerprint, fingerlen );
    if( rc ) { /* that should never happen */
	log_error( "insert_trust_record: keyblock not found: %s\n",
							  g10_errstr(rc) );
	goto leave;
    }

    /* check that we used the primary key (we are little bit paranoid) */
    {	PKT_public_key *a_pk;
	u32 akid[2], bkid[2];

	node = find_kbnode( keyblock, PKT_PUBLIC_KEY );
	a_pk = node->pkt->pkt.public_key;

	/* we can't use cmp_public_keys here because some parts (expiredate)
	 * might not be set in pk <--- but why (fixme) */
	keyid_from_pk( a_pk, akid );
	keyid_from_pk( pk, bkid );

	if( akid[0] != bkid[0] || akid[1] != bkid[1] ) {
	    log_error("did not use primary key for insert_trust_record()\n");
	    rc = G10ERR_GENERAL;
	    goto leave;
	}
    }

    /* We have to look for a shadow dir record which must be reused
     * as the dir record. And: check all signatures which are listed
     * in the hintlist of the shadow dir record.
     */
    rc = tdbio_search_sdir( pk->keyid, pk->pubkey_algo, &shadow );
    if( rc && rc != -1 ) {
	log_error("tdbio_search_dir failed: %s\n", g10_errstr(rc));
	die_invalid_db();
    }
    memset( &dirrec, 0, sizeof dirrec );
    dirrec.rectype = RECTYPE_DIR;
    if( !rc ) {
	/* hey, great: this key has already signed other keys
	 * convert this to a real directory entry */
	hintlist = shadow.r.sdir.hintlist;
	dirrec.recnum = shadow.recnum;
    }
    else {
	dirrec.recnum = tdbio_new_recnum();
    }
    dirrec.r.dir.lid = dirrec.recnum;
    write_record( &dirrec );

    /* store the LID */
    pk->local_id = dirrec.r.dir.lid;
    for( node=keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_PUBLIC_KEY
	    || node->pkt->pkttype == PKT_PUBLIC_SUBKEY ) {
	    PKT_public_key *pk = node->pkt->pkt.public_key;
	    pk->local_id = dirrec.r.dir.lid;
	}
	else if( node->pkt->pkttype == PKT_SIGNATURE ) {
	    PKT_signature *sig = node->pkt->pkt.signature;
	    sig->local_id = dirrec.r.dir.lid;
	}
    }

    /* and put all the other stuff into the keydb */
    rc = update_trust_record( keyblock, NULL );
    if( !rc )
	process_hintlist( hintlist, dirrec.r.dir.lid );

  leave:
    if( rc && hintlist )
	; /* fixme: the hintlist is not anymore anchored */
    release_kbnode( keyblock );
    do_sync();
    return rc;
}


int
update_ownertrust( ulong lid, unsigned new_trust )
{
    TRUSTREC rec;

    read_record( lid, &rec, RECTYPE_DIR );
    rec.r.dir.ownertrust = new_trust;
    write_record( &rec );
    do_sync();
    return 0;
}


