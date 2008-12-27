/************************************************************************
	
	Copyright 2007-2008 Emre Sozer & Patrick Clark Trizila

	Contact: emresozer@freecfd.com , ptrizila@freecfd.com

	This file is a part of Free CFD

	Free CFD is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    any later version.

    Free CFD is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    For a copy of the GNU General Public License,
    see <http://www.gnu.org/licenses/>.

*************************************************************************/
#include<cmath>
#include<iostream>
#include "commons.h"
#define EPS 1e-10
#define INTERPOLATE_POINT 0
#define INTERPOLATE_LINE 1
#define INTERPOLATE_TRI 2
#define INTERPOLATE_TETRA 3

int gelimd(double **a,double *b,double *x, int n);

struct interpolation_tetra {
	int cell[4];
	double weight;
};

struct interpolation_tri {
	int cell[3];
	double weight;
};

extern GridRawData raw;

// Store the cell stencil surronding a node
set<int> stencil;
set<int>::iterator sit,sit1,sit2,sit3;
// A structure to store data about an individual interpolation tetra

// For each node, store possible formations of interpolation tetras
vector<interpolation_tetra> tetras; 
vector<interpolation_tetra>::iterator tet;

vector<interpolation_tri> tris; 
vector<interpolation_tri>::iterator tri;

Vec3D centroid1,centroid2,centroid3,centroid4,ave_centroid;
Vec3D planeNormal;
Vec3D lineDirection;
Vec3D pp,basis1,basis2;
double lengthScale;
int method;
double tolerance=1.e-6;
double weightSum;
double tetra_volume,tri_area,ave_edge,line_distance;
double closeness,skewness,weight;
double maxWeight;
double **a;
double *b;
double *weights;

double distanceMin=1.e20;
int p1,p2;

void Grid::nodeAverages() {
		
	// Loop all the nodes
	for (nit=node.begin();nit!=node.end();nit++) {
		// Initialize stencil to nearest neighbor cells
		for (it=(*nit).cells.begin();it<(*nit).cells.end();it++) stencil.insert(*it);
		// Include nearest ghost cells in the stencil
		for (it=(*nit).ghosts.begin();it!=(*nit).ghosts.end();it++) stencil.insert(-(*it)-1);
		// if the stencil doesn't have at least 4 points, expand it to include 2nd nearest neighbor cells
		// NOTE ideally, second nearest ghosts would also need top be included but it is too much complication
		if (stencil.size()<4) {
			// Loop the cells neighboring the current node
			for (it=(*nit).cells.begin();it!=(*nit).cells.end();it++) {
				// Loop the cells neighboring the current cell
				for (it2=cell[*it].neighborCells.begin();it2!=cell[*it].neighborCells.end();it2++) {
					stencil.insert(*it2);
				}
				// Loop the ghost cells neighboring the current cell
				for (it2=cell[*it].ghosts.begin();it2!=cell[*it].ghosts.end();it2++) {
					stencil.insert(-(*it2)-1);
				}
				
			}
		}
		if (stencil.size()==1) {
			method=INTERPOLATE_POINT;
		} else if (stencil.size()==2) {
			method=INTERPOLATE_LINE;
		} else {
			// If stencil size is larger than 4, potentially we can find at least one interpolation tetra
			// Check if all the points lie on a line (True if a 1D problem)
			// Pick first 2 points in the stencil
			sit=sit2=stencil.begin(); sit1=++sit2; ++sit2;
			centroid1=(*sit>=0) ? cell[*sit].centroid : ghost[-(*sit)-1].centroid;
			centroid2=(*sit1>=0) ? cell[*sit1].centroid : ghost[-(*sit1)-1].centroid;
			// Direction of the line formed by the first 2 points
			lineDirection=(centroid2-centroid1).norm();
			// Loop the rest of the stencil and check if they lie on the same line (true in 1D)
			method=INTERPOLATE_LINE;
			for (;sit2!=stencil.end();sit2++) {
				centroid3=(*sit2>=0) ? cell[*sit2].centroid : ghost[-(*sit2)-1].centroid;
				if ( fabs(((centroid3-centroid1).norm()).dot(lineDirection))>tolerance ) {
					// point 3 doesn't lie on the same line formed by the first 2 points
					// Then at least one interpolation triangle can be formed
					method=INTERPOLATE_TRI;
					break;
				}
			}
			
			if (method==INTERPOLATE_TRI) { // If at least one interpolation triangle can be formed
				// Check if the rest of the points in the stencil lie on the same plane formed by the first three (true in 2D)
				// Average edge length
				lengthScale=0.333*(fabs(centroid2-centroid1)+fabs(centroid3-centroid1)+fabs(centroid3-centroid2));
				// Calculate the normal vector of the plane formed by these 3 points
				planeNormal=(centroid2-centroid1).cross(centroid3-centroid2);
				for (sit3=sit2;sit3!=stencil.end();sit3++) {
					centroid4=(*sit3>=0) ? cell[*sit3].centroid : ghost[-(*sit3)-1].centroid;
					if ( fabs(planeNormal.dot(centroid4-centroid1))/lengthScale>tolerance) {
						// point 4 is not on the same plane
						// Then at least one interpolation tetrahedra can be formed
						method=INTERPOLATE_TETRA;
						break;				
					}
				}
				
			}		
		}
		(*nit).average.clear();
		if (method==INTERPOLATE_TETRA) interpolate_tetra(*nit);
		if (method==INTERPOLATE_TRI) interpolate_tri(*nit);
		if (method==INTERPOLATE_LINE) interpolate_line(*nit);
		if (method==INTERPOLATE_POINT) {
			(*nit).average.insert(pair<int,double>(*(stencil.begin()),1.));
		}
		//cout << tetras.size() << endl;
		
		// Clear temp data
		stencil.clear();
		tetras.clear();
		tris.clear();
		
		//if ((*nit).id % 100 == 0) cout << "\r" << "[I Rank=" << Rank << "] " << float((*nit).id)/grid.nodeCount*100. << "% done" << endl;
	}
	//cout << endl;
}

void Grid::interpolate_tetra(Node& n) {
	
	// Loop through the stencil and construct tetra combinations
	maxWeight=tolerance;
	for (sit=stencil.begin();sit!=stencil.end();sit++) {
		sit1=sit; sit1++;
		for (sit1=sit1;sit1!=stencil.end();sit1++) {
			sit2=sit1; sit2++;
			for (sit2=sit2;sit2!=stencil.end();sit2++) {
				sit3=sit2; sit3++;
				for (sit3=sit3;sit3!=stencil.end();sit3++) {
					centroid1=(*sit>=0) ? cell[*sit].centroid : ghost[-(*sit)-1].centroid;
					centroid2=(*sit1>=0) ? cell[*sit1].centroid : ghost[-(*sit1)-1].centroid;
					centroid3=(*sit2>=0) ? cell[*sit2].centroid : ghost[-(*sit2)-1].centroid;
					centroid4=(*sit3>=0) ? cell[*sit3].centroid : ghost[-(*sit3)-1].centroid;
					tetra_volume=0.5*fabs(((centroid2-centroid1).cross(centroid3-centroid1)).dot(centroid4-centroid1));
					ave_centroid=0.25*(centroid1+centroid2+centroid3+centroid4);
					ave_edge=0.25*(fabs(centroid1-centroid2)+fabs(centroid1-centroid3)+fabs(centroid2-centroid3)
							+fabs(centroid4-centroid1)+fabs(centroid4-centroid2)+fabs(centroid4-centroid3));
					// How close the tetra center to the node for which we are interpolating
					// Normalized by average edge length
					closeness=fabs(n-ave_centroid)/ave_edge;
					closeness=max(closeness,tolerance);
					closeness=1./closeness;
					// If it was an equilateral tetra,skewness should be 1, thus the multiplier here.
					skewness=8.48528137*tetra_volume/((pow(ave_edge,3)));
					weight=closeness*skewness;
					
					if (weight>maxWeight) {
						maxWeight=weight;
						// Declare an interpolation tetra
						interpolation_tetra temp;
						temp.cell[0]=*sit; temp.cell[1]=*sit1; temp.cell[2]=*sit2; temp.cell[3]=*sit3;
						temp.weight=weight;
						tetras.push_back(temp);
					}
					//if (tetras.size()>10) break;
				}
				//if (tetras.size()>10) break;
			}
			//if (tetras.size()>10) break;
		}
		//if (tetras.size()>10) break;
	} // Loop through stencil 
	if (tetras.size()==0) { // This really shouldn't happen
		cerr << "[E] An interpolation tetra couldn't be found for node " << n << endl;
		cerr << "[E] This really shouldn't happen " << n << endl;
		cerr << "[E] Please report this bug !! " << n << endl;
		exit(1);
	} else { // Everyting is normal, go ahead
		// Normalize the weights
		weightSum=0.; 
		for (tet=tetras.begin();tet!=tetras.end();tet++) weightSum+=(*tet).weight;
		for (tet=tetras.begin();tet!=tetras.end();tet++) {
			(*tet).weight/=weightSum;
			// Form the linear system
			a = new double* [4];
 			for (int i=0;i<4;++i) a[i]=new double[4];
 			b = new double[4];
			weights= new double [4];

			for (int i=0;i<4;++i) {
				if ((*tet).cell[i]>=0) {
					for (int j=0;j<3;++j) a[j][i]=cell[(*tet).cell[i]].centroid[j];
				} else {
					for (int j=0;j<3;++j) a[j][i]=ghost[-((*tet).cell[i])-1].centroid[j];	
				}
			}
			
			a[3][0]=1.; a[3][1]=1.; a[3][2]=1.; a[3][3]=1.;
			b[0]=n[0]; b[1]=n[1]; b[2]=n[2]; b[3]=1.;
			// Solve the 4x4 linear system by Gaussion Elimination
			gelimd(a,b,weights,4);
			
			for (int i=0;i<4;++i) {
				if (n.average.find((*tet).cell[i])!=n.average.end()) { // if the cell contributing to the node average is already contained in the map
					n.average[(*tet).cell[i]]+=(*tet).weight*weights[i];
				} else {
					n.average.insert(pair<int,double>((*tet).cell[i],(*tet).weight*weights[i]));
				}
			}
		}
	}
	return;
	
}

void Grid::interpolate_tri(Node& n) {
	
	// Loop through the stencil and construct tri combinations
	maxWeight=tolerance;
	for (sit=stencil.begin();sit!=stencil.end();sit++) {
		sit1=sit; sit1++;
		for (sit1=sit1;sit1!=stencil.end();sit1++) {
			sit2=sit1; sit2++;
			for (sit2=sit2;sit2!=stencil.end();sit2++) {
				centroid1=(*sit>=0) ? cell[*sit].centroid : ghost[-(*sit)-1].centroid;
				centroid2=(*sit1>=0) ? cell[*sit1].centroid : ghost[-(*sit1)-1].centroid;
				centroid3=(*sit2>=0) ? cell[*sit2].centroid : ghost[-(*sit2)-1].centroid;
				tri_area=0.5*fabs((centroid2-centroid1).cross(centroid3-centroid1));
				ave_centroid=1./3.*(centroid1+centroid2+centroid3);
				ave_edge=1./3.*(fabs(centroid1-centroid2)+fabs(centroid1-centroid3)+fabs(centroid2-centroid3));
				// How close the triangle center to the node for which we are interpolating
				// Normalized by average edge length
				closeness=fabs(n-ave_centroid)/ave_edge;
				closeness=max(closeness,tolerance);
				closeness=1./closeness;
				// If it was an equilateral tri,skewness should be 1, thus the multiplier here.
				skewness=2.30940108*tri_area/((pow(ave_edge,2)));
				weight=closeness*skewness;

				if (weight>maxWeight) {
					maxWeight=weight;
					// Declare an interpolation tetra
					interpolation_tri temp;
					temp.cell[0]=*sit; temp.cell[1]=*sit1; temp.cell[2]=*sit2; 
					temp.weight=weight;
					tris.push_back(temp);
				}
					
				
			}
		}
	} // Loop through stencil 
	if (tris.size()==0) { // This really shouldn't happen
		cerr << "[E] An interpolation triangle couldn't be found for node " << n << endl;
		cerr << "[E] This really shouldn't happen " << n << endl;
		cerr << "[E] Please report this bug !! " << n << endl;
		exit(1);
	} else { // Everyting is normal, go ahead
		// Normalize the weights
		weightSum=0.; 
		for (tri=tris.begin();tri!=tris.end();tri++) weightSum+=(*tri).weight;
		for (tri=tris.begin();tri!=tris.end();tri++) {
			(*tri).weight/=weightSum;					
			centroid1= ((*tri).cell[0]>=0) ? cell[(*tri).cell[0]].centroid : ghost[-(*tri).cell[0]-1].centroid;
			centroid2= ((*tri).cell[1]>=0) ? cell[(*tri).cell[1]].centroid : ghost[-(*tri).cell[1]-1].centroid;
			centroid3= ((*tri).cell[2]>=0) ? cell[(*tri).cell[2]].centroid : ghost[-(*tri).cell[2]-1].centroid;
			basis1=(centroid2-centroid1).norm();
			planeNormal=(basis1.cross(centroid3-centroid1)).norm();
			basis2=-1.*(basis1.cross(planeNormal)).norm();
			// Project the node point to the plane
			pp=n-centroid1;
			pp-=pp.dot(planeNormal)*planeNormal;
			pp+=centroid1;
			// Form the linear system
			a = new double* [3];
			for (int i=0;i<3;++i) a[i]=new double[3];
			b = new double[3];
			weights= new double [3];
			a[0][0]=(centroid1).dot(basis1);
			a[0][1]=(centroid2).dot(basis1);
			a[0][2]=(centroid3).dot(basis1);
			a[1][0]=(centroid1).dot(basis2);
			a[1][1]=(centroid2).dot(basis2);
			a[1][2]=(centroid3).dot(basis2);
			a[2][0]=1.;
			a[2][1]=1.;
			a[2][2]=1.;
			b[0]=pp.dot(basis1);
			b[1]=pp.dot(basis2);
			b[2]=1.;
			// Solve the 3x3 linear system by Gaussion Elimination
			gelimd(a,b,weights,3);
			
			for (int i=0;i<3;++i) {
				if (n.average.find((*tri).cell[i])!=n.average.end()) { // if the cell contributing to the node average is already contained in the map
					n.average[(*tri).cell[i]]+=(*tri).weight*weights[i];
				} else {
					n.average.insert(pair<int,double>((*tri).cell[i],(*tri).weight*weights[i]));
				}
			}
		}
	}
	return;
}

void Grid::interpolate_line(Node& n) {
	// Pick the two points in the stencil which are closest to the node
	for (sit=stencil.begin();sit!=stencil.end();sit++) {
		centroid3=(*sit>=0) ? cell[*sit].centroid : ghost[-(*sit)-1].centroid;
		line_distance=fabs(n-centroid3);
		if (line_distance<distanceMin) {
			distanceMin=line_distance;
			p2=p1;
			centroid2=centroid1;
			p1=*sit;
			centroid1=centroid3;
		}
		
	}
	
	lineDirection=(centroid2-centroid1).norm();
	weights= new double [2];
	weights[0]=(n-centroid1).dot(lineDirection)/fabs(centroid2-centroid1);
	weights[1]=1.-weights[0];
	n.average.insert(pair<int,double>(p1,weights[0]));
	n.average.insert(pair<int,double>(p2,weights[1]));
	return;
}

void Grid::faceAverages() {

	unsigned int n;
	map<int,double>::iterator it;
	map<int,double>::iterator fit;

	for (unsigned int f=0;f<faceCount;++f) {
		double weightSumCheck;
		vector<double> nodeWeights;
		// Mid point of the face
		Vec3D mid=0.,patchArea;
		double patchRatio;
		for (unsigned int fn=0;fn<face[f].nodeCount;++fn) {
			nodeWeights.push_back(0.);
			mid+=face[f].node(fn);
		}
		mid/=double(face[f].nodeCount);
		// Get node weights
		int next;
		for (unsigned int fn=0;fn<face[f].nodeCount;++fn) {
			next=fn+1;
			if (next==face[f].nodeCount) next=0;
			patchArea=0.5*(face[f].node(fn)-mid).cross(face[f].node(next)-mid);
			patchRatio=fabs(patchArea)/face[f].area;
			for (unsigned int fnn=0;fnn<face[f].nodeCount;++fnn) {
				nodeWeights[fnn]+=(1./double(3*face[f].nodeCount))*patchRatio;
			}
			nodeWeights[fn]+=patchRatio/3.;
			nodeWeights[next]+=patchRatio/3.;
		}

		face[f].average.clear();
		for (unsigned int fn=0;fn<face[f].nodeCount;++fn) {
			n=face[f].nodes[fn];
			for ( it=node[n].average.begin() ; it != node[n].average.end(); it++ ) {
				if (face[f].average.find((*it).first)!=face[f].average.end()) { // if the cell contributing to the node average is already contained in the face average map
					face[f].average[(*it).first]+=nodeWeights[fn]*(*it).second;
				} else {
					face[f].average.insert(pair<int,double>((*it).first,nodeWeights[fn]*(*it).second));
				}
			}
		}
// 		weightSumCheck=0.;
// 		for ( fit=face[f].average.begin() ; fit != face[f].average.end(); fit++ ) {
// 			//(*fit).second/=double(face[f].nodeCount);
// 			weightSumCheck+=(*fit).second;
// 		}
		// 
// 		cout << nodeWeights.size() << "\t" << weightSumCheck << endl;
		//cout << setw(16) << setprecision(8) << scientific << weightSumCheck << endl;
// 		std::map<int,double>::iterator it;
// 		double avg=0.;
// 		for ( it=face[f].average.begin() ; it != face[f].average.end(); it++ ) {
// 			avg+=(*it).second*cell[(*it).first].rho;
// 		}
// 		if (fabs(avg-2.*face[f].centroid.comp[0]-2.)>1.e-8) cout << 2.*face[f].centroid.comp[0]+2. << "\t" << avg << endl;
	} // end face loop

} // end Grid::faceAverages()


int gelimd(double **a,double *b,double *x, int n)
{
	double tmp,pvt,*t;
	int i,j,k,itmp;

	for (i=0;i<n;i++) {             // outer loop on rows
		pvt = a[i][i];              // get pivot value
		if (fabs(pvt) < EPS) {
			for (j=i+1;j<n;j++) {
				if(fabs(pvt = a[j][i]) >= EPS) break;
			}
			if (fabs(pvt) < EPS) return 1;     // nowhere to run!
			t=a[j];                 // swap matrix rows...
			a[j]=a[i];
			a[i]=t;
			tmp=b[j];               // ...and result vector
			b[j]=b[i];
			b[i]=tmp;
		}
// (virtual) Gaussian elimination of column
		for (k=i+1;k<n;k++) {       // alt: for (k=n-1;k>i;k--)
			tmp = a[k][i]/pvt;
			for (j=i+1;j<n;j++) {   // alt: for (j=n-1;j>i;j--)
				a[k][j] -= tmp*a[i][j];
			}
			b[k] -= tmp*b[i];
		}
	}
// Do back substitution
	for (i=n-1;i>=0;i--) {
		x[i]=b[i];
		for (j=n-1;j>i;j--) {
			x[i] -= a[i][j]*x[j];
		}
		x[i] /= a[i][i];
	}
	return 0;
}

void Grid::nodeAverages_idw() {
	unsigned int c,g;
	double weight=0.;
	Vec3D node2cell,node2ghost;
	map<int,double>::iterator it;

	for (unsigned int n=0;n<nodeCount;++n) {
		node[n].average.clear();
		// Add contributions from real cells
		weightSum=0.;
		for (unsigned int nc=0;nc<node[n].cells.size();++nc) {
			c=node[n].cells[nc];
			node2cell=node[n]-cell[c].centroid;
			weight=1./(node2cell.dot(node2cell));
			//weight=1./fabs(node2cell);
			node[n].average.insert(pair<int,double>(c,weight));
			weightSum+=weight;
		}
		// Add contributions from ghost cells		
		for (unsigned int ng=0;ng<node[n].ghosts.size();++ng) {
			g=node[n].ghosts[ng];
			node2ghost=node[n]-ghost[g].centroid;
			weight=1./(node2ghost.dot(node2ghost));
			//weight=1./fabs(node2ghost);
			node[n].average.insert(pair<int,double>(-g-1,weight));
			weightSum+=weight;
		}
		for ( it=node[n].average.begin() ; it != node[n].average.end(); it++ ) {
			(*it).second/=weightSum;
		}
	}
} // end Grid::nodeAverages_idw()
