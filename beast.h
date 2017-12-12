#ifndef BEAST_H
#define BEAST_H
#include "config.h"
#include "stars.h"
#include "constellations.h"

struct beast_db {
	star_db* stars;
	star_query* results;
	constellation_db* constellations;
	//If no star list is provided, load from catalog
	beast_db() {
		
		stars=new star_db;
		stars->load_catalog();
		results=new star_query(stars);
		
		results->kdmask_filter_catalog();
		results->kdmask_uniform_density(REQUIRED_STARS);
		std::set<constellation,constellation_lt> c_set;
		for (int i=0;i<stars->map_size;i++) if (results->kdmask[i]==0) {
			results->kdsearch(stars->map[i].x,stars->map[i].y,stars->map[i].z,TODO_MAXFOV_D2*2,BRIGHT_THRESH);
			constellation c;
			for (int j=0;j<results->kdresults_size;j++) if (i!=results->kdresults[j] && stars->map[i].photons>=stars->map[results->kdresults[j]].photons){
				c.p=stars->map[i].dist_arcsec(stars->map[results->kdresults[j]]);
				c.s1=i;
				c.s2=results->kdresults[j];
				c_set.insert(c);
			}
			results->undo_kdsearch();
		}
		constellations = new constellation_db;
		constellations->stars=stars;
		constellations->map_size=c_set.size();
		constellations->map=(constellation*)malloc(constellations->map_size*sizeof(constellations->map[0]));
		std::set<constellation>::iterator it = c_set.begin();
		for (int idx=0; idx<constellations->map_size;idx++,it++) {
			constellations->map[idx]=*it;
			constellations->map[idx].idx=idx;
		}
		
		results->reset_kdmask();
		results->reset_kdresults();
		
	}
	
	//Otherwise, this is an image
	beast_db(star_db *s) {
		stars=s;
		results=NULL;
		
		std::sort(stars->map, stars->map+stars->map_size,star_gt_photons);
		int ns=stars->map_size;/* number of stars to check */
		if (ns>REQUIRED_STARS+MAX_FALSE_STARS) ns=REQUIRED_STARS+MAX_FALSE_STARS;
		
		constellations = new constellation_db;
		constellations->stars=stars;
		constellations->map_size=ns*(ns-1)/2;
		constellations->map=(constellation*)malloc(constellations->map_size*sizeof(constellations->map[0]));
		int idx=0;
		for (int j=1;j<ns;j++) for (int i=0;i<j;i++,idx++) {
			constellations->map[idx].p=stars->map[i].dist_arcsec(stars->map[j]);
			constellations->map[idx].s1=i;
			constellations->map[idx].s2=j;
			constellations->map[idx].idx=idx;
		}
		std::sort(constellations->map, constellations->map+constellations->map_size,constellation_lt_p);
	}
	~beast_db() {
		delete stars;
		delete results;
		delete constellations;
	}
};


struct  constellation_score {
	float totalscore;
	int32_t db_id1,db_id2;
	int32_t img_id1,img_id2;
	int *id_map; /* Usage: id_map[newstar]=oldstar */
	float *scores;
	
};
bool constellation_score_gt(const constellation_score &c1, const constellation_score &c2) { return c1.totalscore > c2.totalscore; }

struct constellation_match {
	float R11,R12,R13;
	float R21,R22,R23;
	float R31,R32,R33;
	
	struct constellation_score *c_scores;
	
	int c_scores_size;
	float *winner_scores;
	//winner_id_map[new]=old
	int32_t *winner_id_map;
	float p_match;
	/* weighted_triad results */

	/* see https://en.wikipedia.org/wiki/Triad_method */
	/* and http://nghiaho.com/?page_id=846 */
	/* returns match results */

	/* when compiled, this section contains roughly 430 floating point operations */
	/* according to https://www.karlrupp.net/2016/02/gemm-and-stream-results-on-intel-edison/ */
	/* we can perform >250 MFLOPS with doubles, and >500 MFLOPS with floats */


	void weighted_triad(star db_s1,star db_s2,star img_s1,star img_s2){
		/* v=A*w */
		float wa1=db_s1.x,wa2=db_s1.y,wa3=db_s1.z;
		float wb1=db_s2.x,wb2=db_s2.y,wb3=db_s2.z;
		float va1=img_s1.x,va2=img_s1.y,va3=img_s1.z;
		float vb1=img_s2.x,vb2=img_s2.y,vb3=img_s2.z;
		float wc1=wa2*wb3 - wa3*wb2;
		float wc2=wa3*wb1 - wa1*wb3;
		float wc3=wa1*wb2 - wa2*wb1;
		float wcnorm=sqrt(wc1*wc1+wc2*wc2+wc3*wc3);
		wc1/=wcnorm;
		wc2/=wcnorm;
		wc3/=wcnorm;

		float vc1=va2*vb3 - va3*vb2;
		float vc2=va3*vb1 - va1*vb3;
		float vc3=va1*vb2 - va2*vb1;
		float vcnorm=sqrt(vc1*vc1+vc2*vc2+vc3*vc3);
		vc1/=vcnorm;
		vc2/=vcnorm;
		vc3/=vcnorm;
		
		float vaXvc1=va2*vc3 - va3*vc2;
		float vaXvc2=va3*vc1 - va1*vc3;
		float vaXvc3=va1*vc2 - va2*vc1;

		float waXwc1=wa2*wc3 - wa3*wc2;
		float waXwc2=wa3*wc1 - wa1*wc3;
		float waXwc3=wa1*wc2 - wa2*wc1;
		
		/* some of these are unused */
		float A11=va1*wa1 + vaXvc1*waXwc1 + vc1*wc1;
		/* float A12=va1*wa2 + vaXvc1*waXwc2 + vc1*wc2; */
		/* float A13=va1*wa3 + vaXvc1*waXwc3 + vc1*wc3; */
		float A21=va2*wa1 + vaXvc2*waXwc1 + vc2*wc1;
		/* float A22=va2*wa2 + vaXvc2*waXwc2 + vc2*wc2; */
		/* float A23=va2*wa3 + vaXvc2*waXwc3 + vc2*wc3; */
		float A31=va3*wa1 + vaXvc3*waXwc1 + vc3*wc1;
		float A32=va3*wa2 + vaXvc3*waXwc2 + vc3*wc2;
		float A33=va3*wa3 + vaXvc3*waXwc3 + vc3*wc3;
		
		wc1=-wc1;
		wc2=-wc2;
		wc3=-wc3;
		
		vc1=-vc1;
		vc2=-vc2;
		vc3=-vc3;
		float vbXvc1=vb2*vc3 - vb3*vc2;
		float vbXvc2=vb3*vc1 - vb1*vc3;
		float vbXvc3=vb1*vc2 - vb2*vc1;
		
		float wbXwc1=wb2*wc3 - wb3*wc2;
		float wbXwc2=wb3*wc1 - wb1*wc3;
		float wbXwc3=wb1*wc2 - wb2*wc1;

		/* some of these are unused */
		float B11=vb1*wb1 + vbXvc1*wbXwc1 + vc1*wc1;
		/* float B12=vb1*wb2 + vbXvc1*wbXwc2 + vc1*wc2; */
		/* float B13=vb1*wb3 + vbXvc1*wbXwc3 + vc1*wc3; */
		float B21=vb2*wb1 + vbXvc2*wbXwc1 + vc2*wc1;
		/* float B22=vb2*wb2 + vbXvc2*wbXwc2 + vc2*wc2; */
		/* float B23=vb2*wb3 + vbXvc2*wbXwc3 + vc2*wc3; */
		float B31=vb3*wb1 + vbXvc3*wbXwc1 + vc3*wc1;
		float B32=vb3*wb2 + vbXvc3*wbXwc2 + vc3*wc2;
		float B33=vb3*wb3 + vbXvc3*wbXwc3 + vc3*wc3;
		
		/* use weights based on magnitude */
		/* weighted triad */
		float weightA=1.0/(db_s1.sigma_sq+img_s1.sigma_sq);
		float weightB=1.0/(db_s2.sigma_sq+img_s2.sigma_sq);

		float sumAB=weightA+weightB;
		weightA/=sumAB;
		weightB/=sumAB;
		
		float cz,sz,mz;
		float cy,sy,my;
		float cx,sx,mx;
		
		cz=weightA*A11+weightB*B11;
		sz=weightA*A21+weightB*B21;
		mz=sqrt(cz*cz+sz*sz);
		cz=cz/mz;
		sz=sz/mz;
		
		cy=weightA*sqrt(A32*A32+A33*A33)+weightB*sqrt(B32*B32+B33*B33);
		sy=-weightA*A31-weightB*B31;
		my=sqrt(cy*cy+sy*sy);
		cy=cy/my;
		sy=sy/my;
		
		cx=weightA*A33+weightB*B33;
		sx=weightA*A32+weightB*B32;
		mx=sqrt(cx*cx+sx*sx);
		cx=cx/mx;
		sx=sx/mx;
		
		R11=cy*cz;
		R12=cz*sx*sy - cx*sz;
		R13=sx*sz + cx*cz*sy;
		
		R21=cy*sz;
		R22=cx*cz + sx*sy*sz;
		R23=cx*sy*sz - cz*sx;
		
		R31=-sy;
		R32=cy*sx;
		R33=cx*cy;
	}
	void add_score(constellation *db_const, constellation_score *cs, int32_t *img_mask, beast_db *db, beast_db *img){
		cs->db_id1=db_const->s1;
		cs->db_id2=db_const->s2;
		cs->id_map=(int32_t *)malloc(sizeof(int32_t)*img->stars->map_size);
		cs->scores=(float *)malloc(sizeof(float)*img->stars->map_size);
		for (int i=0;i<img->stars->map_size;i++) cs->id_map[i]=-1;
		for (int i=0;i<img->stars->map_size;i++) cs->scores[i]=0.0;
		
		
		cs->totalscore=log(EXPECTED_FALSE_STARS/(IMG_X*IMG_Y))*(2*img->stars->map_size);
		db->results->kdsearch(R11,R12,R13,TODO_MAXFOV_D2,BRIGHT_THRESH);
		for(int32_t i=0;i<db->results->kdresults_size;i++){
			int32_t o=db->results->kdresults[i];
			star s=db->stars->map[o];
			float x=s.x*R11+s.y*R12+s.z*R13;
			float y=s.x*R21+s.y*R22+s.z*R23;
			float z=s.x*R31+s.y*R32+s.z*R33;
			float px=y/(x*PIXX_TANGENT);
			float py=z/(x*PIXY_TANGENT);
			int nx,ny;
			nx=(int)(px+IMG_X/2.0f);
			ny=(int)(py+IMG_Y/2.0f);
			int32_t n=-1;
			if (nx==-1) nx++;
			else if (nx==IMG_X) nx--;
			if (ny==-1) ny++;
			else if (ny==IMG_Y) ny--;
			if (nx>=0&&nx<IMG_X&&ny>=0&&ny<IMG_Y) n=img_mask[nx+ny*IMG_X];
			if (n!=-1) {
				float sigma_sq=img->stars->map[n].sigma_sq+db->stars->max_variance;
				float maxdist_sq=-sigma_sq*(log(sigma_sq)+MATCH_VALUE);
				float a=(px-img->stars->map[n].px);
				float b=(py-img->stars->map[n].py);
				float score = (maxdist_sq-(a*a+b*b))/(2*sigma_sq);
				/* only match the closest star */
				if (score>cs->scores[n]){
					cs->id_map[n]=o;
					cs->scores[n]=score;
				}
			}
		}
		db->results->undo_kdsearch();
		for(int n=0;n<img->stars->map_size;n++) {
			cs->totalscore+=cs->scores[n];
		}
		
	}
	constellation_match(beast_db *db, beast_db *img) {
		c_scores=NULL;
		c_scores_size=0;
		/* Do we have enough stars? */
		if (db->stars->map_size<2||img->stars->map_size<2) return;

		winner_id_map=(int *)malloc(sizeof(int)*img->stars->map_size);
		winner_scores=(float *)malloc(sizeof(float)*img->stars->map_size);
		
		int32_t *img_mask = img->stars->get_img_mask(db->stars->max_variance);
		
		for (int n=0;n<img->constellations->map_size;n++) {
			constellation lb=img->constellations->map[n];
			constellation ub=img->constellations->map[n];
			lb.p-=POS_ERR_SIGMA*PIXSCALE*sqrt(img->stars->map[lb.s1].sigma_sq+img->stars->map[lb.s2].sigma_sq+2*db->stars->max_variance);
			ub.p+=POS_ERR_SIGMA*PIXSCALE*sqrt(img->stars->map[ub.s1].sigma_sq+img->stars->map[ub.s2].sigma_sq+2*db->stars->max_variance);
			constellation *lower=std::lower_bound (db->constellations->map, db->constellations->map+db->constellations->map_size, lb,constellation_lt_p);	
			constellation *upper=std::upper_bound (db->constellations->map, db->constellations->map+db->constellations->map_size, ub,constellation_lt_p);
			//rewind by one
			upper--;
			
			//TODO: get rid of cscores 
			if (lower->idx<=upper->idx) c_scores=(struct constellation_score*)realloc(c_scores,sizeof(struct constellation_score)*(c_scores_size+(upper->idx-lower->idx+1)*2));
			for (int o=lower->idx;o<=upper->idx;o++) {
				int32_t db_idx1=db->constellations->map[o].s1;
				int32_t db_idx2=db->constellations->map[o].s2;
				int32_t img_idx1=img->constellations->map[n].s1;
				int32_t img_idx2=img->constellations->map[n].s2;
				
				star db_s1=db->stars->map[db_idx1];
				star db_s2=db->stars->map[db_idx2];
				star img_s1=img->stars->map[img_idx1];
				star img_s2=img->stars->map[img_idx2];
				
				/* try both orderings of stars */
				weighted_triad(db_s1,db_s2,img_s1,img_s2);
				c_scores[c_scores_size].img_id1=img_idx1;
				c_scores[c_scores_size].img_id2=img_idx2;
				add_score(&(db->constellations->map[o]),&c_scores[c_scores_size],img_mask,db,img);
				c_scores_size++;
				
				weighted_triad(db_s1,db_s2,img_s2,img_s1);
				c_scores[c_scores_size].img_id1=img_idx2;
				c_scores[c_scores_size].img_id2=img_idx1;
				add_score(&(db->constellations->map[o]),&c_scores[c_scores_size],img_mask,db,img);
				c_scores_size++;
			}
		}
		std::sort(c_scores, c_scores+c_scores_size,constellation_score_gt);
		for (int i=0;i<img->stars->map_size;i++) {winner_id_map[i]=-1;winner_scores[i]=0.0f;}
		if (c_scores_size>0) {
			for(int n=0;n<img->stars->map_size;n++) {
				int o=c_scores[0].id_map[n];
				if (o!=-1){
					winner_scores[img->stars->map[n].star_idx]=c_scores[0].scores[n];
					winner_id_map[img->stars->map[n].star_idx]=db->stars->map[o].id;
				}
			}
			
		}
		//TODO: move to add_score
		/* add up probabilities of all matches, excluding those which */
		/* are equivalent to the best match (S1==s1,S2=s2) */
		p_match=1.0;
		//printf("size %lu\n",c_scores_size*sizeof(c_scores[0]));
		if (c_scores_size>0) {
			float bestscore=c_scores[0].totalscore;
			int db_id1=c_scores[0].db_id1;
			int db_id2=c_scores[0].db_id2;
			int img_id1=c_scores[0].img_id1;
			int img_id2=c_scores[0].img_id2;
			/* set attitude matrix to best match */
			weighted_triad(db->stars->map[db_id1],db->stars->map[db_id2],img->stars->map[img_id1],img->stars->map[img_id2]);
			for(int i=1;i<c_scores_size;i++) {
				if (c_scores[i].id_map[img_id1]!=db_id1&&c_scores[i].id_map[img_id2]!=db_id2){
					p_match+=exp(c_scores[i].totalscore-bestscore);
				}
			}
			//Turns out baysian hypothesis testing was the best way
			//after all. Who would've guessed?
			p_match=1.0/p_match;
		} else {
			p_match=0.0;
		}
		free(img_mask);
	}
	~constellation_match() {
		for (int i=0;i<c_scores_size; i++) {
			free(c_scores[i].scores);
			free(c_scores[i].id_map);
		}
		free(c_scores);
		free(winner_id_map);
		free(winner_scores);
	}
};
#endif