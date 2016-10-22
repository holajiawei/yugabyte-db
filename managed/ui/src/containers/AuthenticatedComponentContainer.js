// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import AuthenticatedComponent from '../components/AuthenticatedComponent';
import { fetchUniverseList, fetchUniverseListSuccess,
  fetchUniverseListFailure, resetUniverseList
} from '../actions/universe';
import { getRegionList, getRegionListSuccess,
  getRegionListFailure, getProviderList,
  getProviderListSuccess, getProviderListFailure }
  from '../actions/cloud';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchUniverseList: () => {
      dispatch(fetchUniverseList())
        .then((response) => {
          if (response.payload.status !== 200) {
            dispatch(fetchUniverseListFailure(response.payload));
          } else {
            dispatch(fetchUniverseListSuccess(response.payload));
          }
        });
    },

    getProviderListItems: () => {
      dispatch(getProviderList()).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(getProviderListFailure(response.payload));
        } else {
          dispatch(getProviderListSuccess(response.payload));
          response.payload.data.forEach(function (item, idx) {
            dispatch(getRegionList(item.uuid, true))
              .then((response) => {
                if (response.payload.status !== 200) {
                  dispatch(getRegionListFailure(response.payload));
                } else {
                  dispatch(getRegionListSuccess(response.payload));
                }
              });
          })}
      });
    },

    resetUniverseList: () => {
      dispatch(resetUniverseList());
    }

  }
}

const mapStateToProps = (state) => {
  return {
    customer: state.customer,
    universe: state.universe,
    cloud: state.cloud
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(AuthenticatedComponent);